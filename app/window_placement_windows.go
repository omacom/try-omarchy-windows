package main

import (
	"syscall"
	"unsafe"
)

var (
	procGetWindowPlacement  = user32.NewProc("GetWindowPlacement")
	procSetWindowPlacement  = user32.NewProc("SetWindowPlacement")
	procEnumDisplayMonitors = user32.NewProc("EnumDisplayMonitors")
	// One callback for the process: syscall.NewCallback never frees its slot.
	enumMonitorsCallback = syscall.NewCallback(enumMonitorsProc)
	enumMonitorsResult   []screenRect
)

const (
	swShowNormal    = 1
	swShowMinimized = 2
	swShowMaximized = 3
)

type windowPlacementStruct struct {
	length, flags, showCmd uint32
	minPosition            [2]int32
	maxPosition            [2]int32
	normalPosition         screenRect
}

func enumMonitorsProc(_, _ uintptr, rect *screenRect, _ uintptr) uintptr {
	enumMonitorsResult = append(enumMonitorsResult, *rect)
	return 1
}

// monitorRects lists the virtual-screen rectangles of every display. Only the
// title enforcer's goroutine calls it, so the shared result slice needs no lock.
func monitorRects() []screenRect {
	enumMonitorsResult = nil
	procEnumDisplayMonitors.Call(0, 0, enumMonitorsCallback, 0)
	return append([]screenRect(nil), enumMonitorsResult...)
}

// capturePlacement reads where the VM window is now. Minimized windows are
// skipped so a launch never restores into the taskbar.
func capturePlacement(hwnd uintptr) *windowPlacement {
	var wp windowPlacementStruct
	wp.length = uint32(unsafe.Sizeof(wp))
	if r, _, _ := procGetWindowPlacement.Call(hwnd, uintptr(unsafe.Pointer(&wp))); r == 0 || wp.showCmd == swShowMinimized {
		return nil
	}
	return &windowPlacement{Normal: wp.normalPosition, Maximized: wp.showCmd == swShowMaximized}
}

// applyPlacement moves the VM window to the remembered rectangle and state.
func applyPlacement(hwnd uintptr, p *windowPlacement) bool {
	var wp windowPlacementStruct
	wp.length = uint32(unsafe.Sizeof(wp))
	wp.showCmd = swShowNormal
	if p.Maximized {
		wp.showCmd = swShowMaximized
	}
	wp.normalPosition = p.Normal
	r, _, _ := procSetWindowPlacement.Call(hwnd, uintptr(unsafe.Pointer(&wp)))
	return r != 0
}

// rememberedWindow returns the placement to restore this launch, or nil for
// the maximized default.
func rememberedWindow(dir string) *windowPlacement {
	p, err := loadWindowPlacement(dir)
	if err != nil {
		logf("ignoring %s: %v", windowPlacementFilename, err)
		return nil
	}
	if !p.usable(monitorRects()) {
		return nil
	}
	return p
}
