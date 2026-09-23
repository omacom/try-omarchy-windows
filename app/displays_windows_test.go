//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestFullscreenDisplaySelection(t *testing.T) {
	monitors := []hostMonitor{
		{Name: `\\.\DISPLAY2`, Bounds: screenRect{1920, 0, 4480, 1440}},
		{Name: `\\.\DISPLAY1`, Bounds: screenRect{0, 0, 1920, 1080}, Primary: true},
	}
	for _, tc := range []struct {
		name string
		want int
	}{
		{"", 1},
		{`\\.\DISPLAY2`, 0},
		{`\\.\DISPLAY9`, 1},
	} {
		index, monitor := selectedHostMonitor(tc.name, monitors)
		if index != tc.want || monitor != monitors[tc.want] {
			t.Fatalf("selection %q = %d, %+v", tc.name, index, monitor)
		}
	}
}

func TestFullscreenDisplayEnumeration(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires interactive Windows desktop")
	}
	monitors := hostMonitors()
	if len(monitors) == 0 {
		t.Fatal("no host displays")
	}
	_, primary := selectedHostMonitor("", monitors)
	if !primary.Primary || primary.Bounds.width() <= 0 || primary.Bounds.height() <= 0 {
		t.Fatalf("invalid primary display: %+v", primary)
	}
	if width, height := fullscreenTargetSize(primary.Name); width != int(primary.Bounds.width()) || height != int(primary.Bounds.height()) {
		t.Fatalf("fullscreen size %d x %d, display %+v", width, height, primary)
	}
}

func TestNativeQEMUPrimaryWindow(t *testing.T) {
	tool := os.Getenv("QEMU_SYSTEM")
	if tool == "" || os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires QEMU_SYSTEM and an interactive Windows desktop")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	cfg := &config{displays: 3, displayWidth: 800, displayHeight: 600}
	cmd := exec.Command(tool, "-machine", "q35", "-accel", "tcg", "-m", "128", "-nodefaults", "-S", "-device", displayDevice(cfg, 256<<20), "-display", "sdl,gl=off,window-close=off", "-name", appTitle)
	var detail diskToolErrors
	cmd.Stderr = &detail
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait(); qemuPid.Store(0); qemuHwnd.Store(0); enumTitlePid = 0 }()
	qemuPid.Store(uint32(cmd.Process.Pid))
	dir := t.TempDir()
	deadline := time.Now().Add(15 * time.Second)
	for {
		enforceDisplayWindows(qemuPid.Load(), dir, false, "", 0)
		if len(enumTitleWindows) == 1 {
			break
		}
		if time.Now().After(deadline) {
			cmd.Process.Kill()
			cmd.Wait()
			t.Fatalf("expected primary display window, got %d: %s", len(enumTitleWindows), detail.String())
		}
		time.Sleep(100 * time.Millisecond)
	}
	found := map[int]bool{}
	for hwnd, state := range enumTitleWindows {
		if !isQemuDisplayWindow(hwnd, qemuPid.Load()) {
			t.Fatal("close guard cannot identify display")
		}
		found[state.index] = true
		var title [256]uint16
		procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&title[0])), 256)
		if syscall.UTF16ToString(title[:]) == "" {
			t.Fatal("unbranded display")
		}
		if _, err := loadDisplayPlacement(dir, state.index); err != nil {
			t.Fatal(err)
		}
	}
	if len(found) != 1 || !found[0] {
		t.Fatalf("unstable output identities: %v", found)
	}
	if qemuHwnd.Load() == 0 {
		t.Fatal("no active display for tray")
	}
}

// Secondary SDL windows appear when the guest activates their scanouts. Exercise
// the native window lifecycle independently of guest boot and acceleration.
func TestMultipleNativeDisplayWindowLifecycle(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	type windowClass struct {
		size, style                   uint32
		callback                      uintptr
		classExtra, windowExtra       int32
		instance, icon, cursor, brush uintptr
		menu, class                   *uint16
		smallIcon                     uintptr
	}
	class, _ := syscall.UTF16PtrFromString("SDL_app")
	instance, _, _ := procGetModuleHandleW.Call(0)
	callback := syscall.NewCallback(func(hwnd, message, w, l uintptr) uintptr {
		r, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
		return r
	})
	wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: callback, instance: instance, class: class}
	if result, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); result == 0 {
		t.Fatal(err)
	}
	var windows []uintptr
	defer func() {
		for _, hwnd := range windows {
			procDestroyWindow.Call(hwnd)
		}
		qemuPid.Store(0)
		qemuHwnd.Store(0)
		enumTitlePid = 0
	}()
	for index := 0; index < 3; index++ {
		title, _ := syscall.UTF16PtrFromString(fmt.Sprintf("QEMU (%s-%d)", appTitle, index))
		hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), 0x00cf0000|wsVisible, 100, 100, 800, 600, 0, 0, instance, 0)
		if hwnd == 0 {
			t.Fatal(err)
		}
		windows = append(windows, hwnd)
	}
	qemuPid.Store(uint32(os.Getpid()))
	dir := t.TempDir()
	enforceDisplayWindows(qemuPid.Load(), dir, false, "", 0)
	if len(enumTitleWindows) != 3 {
		t.Fatalf("lost secondary display windows: %d", len(enumTitleWindows))
	}
	for index, hwnd := range windows {
		state := enumTitleWindows[hwnd]
		if state == nil || state.index != index || !isQemuDisplayWindow(hwnd, qemuPid.Load()) {
			t.Fatalf("incorrect display identity %d", index)
		}
		if placement, err := loadDisplayPlacement(dir, index); err != nil || placement == nil {
			t.Fatalf("missing saved placement %d: %v", index, err)
		}
	}
	// Simulate removal of a monitor while a live output is offscreen.
	procShowWindow.Call(windows[1], swShowNormal)
	procSetWindowPos.Call(windows[1], 0, 30000, 30000, 800, 600, 0x0004|0x0010)
	monitors := monitorRects()
	if capturePlacement(windows[1]).usable(monitors) {
		t.Fatal("test window did not move offscreen")
	}
	enforceDisplayWindows(qemuPid.Load(), dir, false, "", 0)
	if capturePlacement(windows[1]).usable(monitors) {
		t.Fatal("moved a window without a topology change")
	}
	enumTitleMonitors = []screenRect{{30000, 30000, 32000, 32000}}
	enforceDisplayWindows(qemuPid.Load(), dir, false, "", 0)
	if !capturePlacement(windows[1]).usable(monitors) {
		t.Fatal("lost output after monitor removal")
	}
	procDestroyWindow.Call(windows[2])
	windows = windows[:2]
	enforceDisplayWindows(qemuPid.Load(), dir, false, "", 0)
	if len(enumTitleWindows) != 2 {
		t.Fatal("retained a closed display window")
	}
}
