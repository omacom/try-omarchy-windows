package main

import (
	"sync"
	"syscall"
	"unsafe"
)

var (
	procGetWindowRect       = user32.NewProc("GetWindowRect")
	procGetWindowPlacement  = user32.NewProc("GetWindowPlacement")
	procSetWindowPlacement  = user32.NewProc("SetWindowPlacement")
	procEnumDisplayMonitors = user32.NewProc("EnumDisplayMonitors")
	procGetMonitorInfoW     = user32.NewProc("GetMonitorInfoW")
	procMonitorFromWindow   = user32.NewProc("MonitorFromWindow")
	// One callback for the process: syscall.NewCallback never frees its slot.
	enumMonitorsCallback = syscall.NewCallback(enumMonitorsProc)
	enumMonitorDetails   []hostMonitor
	monitorEnumerationMu sync.Mutex
)

type monitorInfoEx struct {
	Size   uint32
	Bounds screenRect
	Work   screenRect
	Flags  uint32
	Device [32]uint16
}

const (
	swShowNormal     = 1
	swShowMinimized  = 2
	swShowMaximized  = 3
	swShowNoActivate = 4
	swShowNA         = 8
	monitorNearest   = 2
)

type windowPlacementStruct struct {
	length, flags, showCmd uint32
	minPosition            [2]int32
	maxPosition            [2]int32
	normalPosition         screenRect
}

func enumMonitorsProc(monitor, _ uintptr, rect *screenRect, _ uintptr) uintptr {
	info := monitorInfoEx{Size: uint32(unsafe.Sizeof(monitorInfoEx{}))}
	if ok, _, _ := procGetMonitorInfoW.Call(monitor, uintptr(unsafe.Pointer(&info))); ok != 0 {
		enumMonitorDetails = append(enumMonitorDetails, hostMonitor{syscall.UTF16ToString(info.Device[:]), info.Bounds, info.Flags&1 != 0, info.Work})
	} else {
		enumMonitorDetails = append(enumMonitorDetails, hostMonitor{Bounds: *rect, Work: *rect})
	}
	return 1
}

// Enumeration is shared by boot planning, Settings and the title enforcer.
func monitorRects() []screenRect {
	monitors := hostMonitors()
	rects := make([]screenRect, 0, len(monitors))
	for _, m := range monitors {
		rects = append(rects, m.Bounds)
	}
	return rects
}

func hostMonitors() []hostMonitor {
	monitorEnumerationMu.Lock()
	defer monitorEnumerationMu.Unlock()
	enumMonitorDetails = nil
	procEnumDisplayMonitors.Call(0, 0, enumMonitorsCallback, 0)
	// Enumeration order is not a promise that the primary display is first.
	return primaryFirstMonitors(enumMonitorDetails)
}

// hostMonitorForWindow returns the monitor nearest the window. The cursor
// guard uses it so a fullscreen display window is clipped to the display it
// actually occupies, including one to the left of or above the primary.
func hostMonitorForWindow(hwnd uintptr) (hostMonitor, bool) {
	monitor, _, _ := procMonitorFromWindow.Call(hwnd, monitorNearest)
	if monitor == 0 {
		return hostMonitor{}, false
	}
	info := monitorInfoEx{Size: uint32(unsafe.Sizeof(monitorInfoEx{}))}
	if ok, _, _ := procGetMonitorInfoW.Call(monitor, uintptr(unsafe.Pointer(&info))); ok == 0 {
		return hostMonitor{}, false
	}
	host := hostMonitor{syscall.UTF16ToString(info.Device[:]), info.Bounds, info.Flags&1 != 0, info.Work}
	if host.Bounds.width() <= 0 || host.Bounds.height() <= 0 {
		return hostMonitor{}, false
	}
	return host, true
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

// correctPlacement repairs an existing window without activating it. The
// ordinary boot/shutdown correction only applies to restored windows.
func correctPlacement(hwnd uintptr, p *windowPlacement) bool {
	if p == nil {
		return false
	}
	var wp windowPlacementStruct
	wp.length = uint32(unsafe.Sizeof(wp))
	if r, _, _ := procGetWindowPlacement.Call(hwnd, uintptr(unsafe.Pointer(&wp))); r == 0 || wp.showCmd == swShowMinimized {
		return false
	}
	wp.normalPosition = p.Normal
	wp.showCmd = swShowNoActivate
	if p.Maximized {
		wp.showCmd = swShowNA // preserve the current maximized state
	}
	r, _, _ := procSetWindowPlacement.Call(hwnd, uintptr(unsafe.Pointer(&wp)))
	return r != 0
}
