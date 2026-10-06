//go:build windows

package main

import (
	"os"
	"runtime"
	"sync"
	"syscall"
	"testing"
	"unsafe"
)

func TestMinimizedTopologyRepairWaitsForRestore(t *testing.T) {
	monitors := []hostMonitor{{Bounds: screenRect{0, 0, 1920, 1080}, Work: screenRect{0, 0, 1920, 1040}}}
	state := &displayWindowState{topologyRepair: true}
	calls := 0
	apply := func(p *windowPlacement) bool { calls++; return true }
	if state.repairTopology(nil, false, monitors, apply) != nil || !state.topologyRepair || calls != 0 {
		t.Fatal("consumed a repair while minimized")
	}
	now := &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}
	if state.repairTopology(now, true, monitors, apply) != nil || !state.topologyRepair || calls != 0 {
		t.Fatal("consumed a repair while dragging")
	}
	if state.repairTopology(now, false, monitors, func(*windowPlacement) bool { return false }) != nil || !state.topologyRepair {
		t.Fatal("consumed a failed repair")
	}
	restored := state.repairTopology(now, false, monitors, apply)
	if restored == nil || state.topologyRepair || calls != 1 || !usablePlacement(restored, monitors) || !state.target.sameAs(restored) {
		t.Fatal("lost deferred repair after restore")
	}
}

func TestDestroyedWindowClearsHookStateBeforeLiveFiltering(t *testing.T) {
	const hwnd = uintptr(0x123456)
	defer clearWindowEventState(hwnd)
	defer destroyedWindows.Delete(hwnd)
	retitledDisplays.Store(hwnd, recordedDisplay{123, 2, false})
	draggedWindows.Store(hwnd, uint32(123))
	userMovedWindows.Store(hwnd, uint32(123))
	qemuWindowEvent(0, eventObjectDestroy, hwnd, objidWindow, 1, 0, 0)
	if _, ok := draggedWindows.Load(hwnd); !ok {
		t.Fatal("a child-object event removed the window")
	}
	qemuWindowEvent(0, eventObjectDestroy, hwnd, objidWindow, 0, 0, 0)
	for _, states := range []*sync.Map{&retitledDisplays, &draggedWindows, &userMovedWindows} {
		if _, ok := states.Load(hwnd); ok {
			t.Fatal("destroyed window retained hook state")
		}
	}
	if _, ok := destroyedWindows.Load(hwnd); !ok {
		t.Fatal("destroyed window retained enforcer state")
	}
}

func TestHookStatePruningKeepsOnlyCurrentLiveWindows(t *testing.T) {
	const old, current, gone = uintptr(0x111), uintptr(0x222), uintptr(0x333)
	defer clearWindowEventState(old)
	defer clearWindowEventState(current)
	defer clearWindowEventState(gone)
	retitledDisplays.Store(old, recordedDisplay{1, 0, false})
	draggedWindows.Store(old, uint32(1))
	userMovedWindows.Store(old, uint32(1))
	retitledDisplays.Store(current, recordedDisplay{2, 1, false})
	draggedWindows.Store(current, uint32(2))
	userMovedWindows.Store(current, uint32(2))
	retitledDisplays.Store(gone, recordedDisplay{2, 2, false})
	draggedWindows.Store(gone, uint32(2))
	userMovedWindows.Store(gone, uint32(2))
	pruneWindowEventState(2, func(hwnd uintptr, pid uint32) bool { return hwnd == current && pid == 2 })
	for _, states := range []*sync.Map{&retitledDisplays, &draggedWindows, &userMovedWindows} {
		if _, ok := states.Load(old); ok {
			t.Fatal("replacement runtime retained old state")
		}
		if _, ok := states.Load(gone); ok {
			t.Fatal("missed destroy event retained state")
		}
		if _, ok := states.Load(current); !ok {
			t.Fatal("pruning discarded a live window")
		}
	}
}

func TestNativePlacementCorrectionKeepsForeground(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	class, _ := syscall.UTF16PtrFromString("STATIC")
	title, _ := syscall.UTF16PtrFromString("Placement correction test")
	instance, _, _ := procGetModuleHandleW.Call(0)
	makeWindow := func() uintptr {
		hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), 0x00cf0000, 100, 100, 800, 600, 0, 0, instance, 0)
		if hwnd == 0 {
			t.Fatal(err)
		}
		t.Cleanup(func() { procDestroyWindow.Call(hwnd) })
		return hwnd
	}
	vm, other := makeWindow(), makeWindow()
	for _, maximized := range []bool{false, true} {
		p := &windowPlacement{Normal: screenRect{100, 100, 900, 700}, Maximized: maximized}
		if !applyPlacement(vm, p) {
			t.Fatal("initial placement failed")
		}
		procShowWindow.Call(other, swShowNormal)
		procSetForegroundWindow.Call(other)
		foreground, _, _ := procGetForegroundWindow.Call()
		if foreground != other {
			t.Fatal("could not establish the foreground control window")
		}
		p.Normal = screenRect{150, 150, 1000, 750}
		if !correctPlacement(vm, p) {
			t.Fatal("correction failed")
		}
		if foreground, _, _ := procGetForegroundWindow.Call(); foreground != other {
			t.Fatal("correction activated the VM")
		}
		if now := capturePlacement(vm); !now.sameAs(p) {
			t.Fatalf("correction changed show state or lost bounds: %+v, want %+v", now, p)
		}
	}
	procShowWindow.Call(vm, swShowMinimized)
	if correctPlacement(vm, &windowPlacement{Normal: screenRect{150, 150, 1000, 750}}) || capturePlacement(vm) != nil {
		t.Fatal("correction restored a minimized window")
	}
}
