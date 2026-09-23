package main

import (
	"syscall"
	"unsafe"
)

var (
	procGetWindowRect       = user32.NewProc("GetWindowRect")
	procGetWindowPlacement  = user32.NewProc("GetWindowPlacement")
	procSetWindowPlacement  = user32.NewProc("SetWindowPlacement")
	procEnumDisplayMonitors = user32.NewProc("EnumDisplayMonitors")
	procGetMonitorInfoW     = user32.NewProc("GetMonitorInfoW")
	// One callback for the process: syscall.NewCallback never frees its slot.
	enumMonitorsCallback = syscall.NewCallback(enumMonitorsProc)
	enumMonitorsResult   []screenRect
	enumMonitorDetails   []hostMonitor
)

type hostMonitor struct {
	Name    string
	Bounds  screenRect
	Primary bool
}

type monitorInfoEx struct {
	Size   uint32
	Bounds screenRect
	Work   screenRect
	Flags  uint32
	Device [32]uint16
}

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

func enumMonitorsProc(monitor, _ uintptr, rect *screenRect, _ uintptr) uintptr {
	enumMonitorsResult = append(enumMonitorsResult, *rect)
	info := monitorInfoEx{Size: uint32(unsafe.Sizeof(monitorInfoEx{}))}
	if ok, _, _ := procGetMonitorInfoW.Call(monitor, uintptr(unsafe.Pointer(&info))); ok != 0 {
		enumMonitorDetails = append(enumMonitorDetails, hostMonitor{syscall.UTF16ToString(info.Device[:]), info.Bounds, info.Flags&1 != 0})
	} else {
		enumMonitorDetails = append(enumMonitorDetails, hostMonitor{Bounds: *rect})
	}
	return 1
}

// monitorRects lists the virtual-screen rectangles of every display. Only the
// title enforcer's goroutine calls it, so the shared result slice needs no lock.
func monitorRects() []screenRect {
	enumMonitorsResult = nil
	enumMonitorDetails = nil
	procEnumDisplayMonitors.Call(0, 0, enumMonitorsCallback, 0)
	return append([]screenRect(nil), enumMonitorsResult...)
}

func hostMonitors() []hostMonitor {
	monitorRects()
	return append([]hostMonitor(nil), enumMonitorDetails...)
}

// A missing selected display falls back to the primary display. Keep the
// saved device name so reconnecting the monitor restores the user's choice.
func selectedHostMonitor(name string, monitors []hostMonitor) (int, hostMonitor) {
	primary := 0
	for i, monitor := range monitors {
		if monitor.Primary {
			primary = i
		}
		if name != "" && name == monitor.Name {
			return i, monitor
		}
	}
	if len(monitors) == 0 {
		return 0, hostMonitor{}
	}
	return primary, monitors[primary]
}

func fullscreenTargetSize(name string) (int, int) {
	_, monitor := selectedHostMonitor(name, hostMonitors())
	if monitor.Bounds.width() > 0 && monitor.Bounds.height() > 0 {
		return int(monitor.Bounds.width()), int(monitor.Bounds.height())
	}
	return screenSize(true)
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
