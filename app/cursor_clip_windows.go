//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var procShAppBarMessage = syscall.NewLazyDLL("shell32.dll").NewProc("SHAppBarMessage")

const (
	abmGetAutoHideBarEx = 0x0000000B
	abeLeft             = 0
	abeTop              = 1
	abeRight            = 2
	abeBottom           = 3

	smXvirtualScreen  = 76
	smYvirtualScreen  = 77
	smCxvirtualScreen = 78
	smCyvirtualScreen = 79
)

type appBarData struct {
	cbSize           uint32
	hWnd             uintptr
	uCallbackMessage uint32
	uEdge            uint32
	rc               screenRect
	lParam           uintptr
}

// autoHideTaskbarRect reports an auto-hidden taskbar on an edge of monitor.
// ABM_GETAUTOHIDEBAREX is the multiple-monitor-aware query, so a taskbar on
// another display does not affect this display. The one-pixel rectangle is
// enough for fullscreenCursorClip; the real taskbar bounds are not needed
// because the guard only keeps the pointer away from the triggering edge.
func autoHideTaskbarRect(monitor screenRect) (screenRect, bool) {
	for _, edge := range []uint32{abeLeft, abeTop, abeRight, abeBottom} {
		data := appBarData{
			cbSize: uint32(unsafe.Sizeof(appBarData{})),
			uEdge:  edge,
			rc:     monitor,
		}
		result, _, _ := procShAppBarMessage.Call(abmGetAutoHideBarEx, uintptr(unsafe.Pointer(&data)))
		if result == 0 {
			continue
		}
		switch edge {
		case abeTop:
			return screenRect{Left: monitor.Left, Top: monitor.Top, Right: monitor.Right, Bottom: monitor.Top + 1}, true
		case abeRight:
			return screenRect{Left: monitor.Right - 1, Top: monitor.Top, Right: monitor.Right, Bottom: monitor.Bottom}, true
		case abeBottom:
			return screenRect{Left: monitor.Left, Top: monitor.Bottom - 1, Right: monitor.Right, Bottom: monitor.Bottom}, true
		default:
			return screenRect{Left: monitor.Left, Top: monitor.Top, Right: monitor.Left + 1, Bottom: monitor.Bottom}, true
		}
	}
	return screenRect{}, false
}

// virtualScreenRect is the bounding box of every monitor, which is the space
// the pointer may roam. Clipping to it (instead of one monitor) keeps the
// cursor able to cross to other displays.
func virtualScreenRect() (screenRect, bool) {
	metric := func(index int) int32 {
		value, _, _ := procGetSystemMetrics.Call(uintptr(index))
		return int32(value)
	}
	virtual := screenRect{metric(smXvirtualScreen), metric(smYvirtualScreen), 0, 0}
	virtual.Right = virtual.Left + metric(smCxvirtualScreen)
	virtual.Bottom = virtual.Top + metric(smCyvirtualScreen)
	if virtual.width() <= 0 || virtual.height() <= 0 {
		return screenRect{}, false
	}
	return virtual, true
}

// updateQemuCursorClip gives a fullscreen VM an edge-aware clip only while SDL
// reports that its grab is active; windowed mode keeps the plain release
// behavior. Fullscreen is decided from the window's own rectangle every tick
// so a runtime Ctrl+Alt+F toggle takes effect without relaunching, including
// the return to windowed use. QEMU's Ctrl+Alt+G changes the window title
// without the exit-grab suffix, which lets the guard honor the documented
// release chord instead of immediately re-confining the pointer.
func updateQemuCursorClip() {
	pid := qemuPid.Load()
	if pid == 0 || foregroundPid() != pid {
		return
	}
	hwnd, _, _ := procGetForegroundWindow.Call()
	if hwnd == 0 || !recordedDisplayGrab(hwnd) {
		procClipCursor.Call(0)
		return
	}
	monitor, ok := hostMonitorForWindow(hwnd)
	if !ok {
		procClipCursor.Call(0)
		return
	}
	var window screenRect
	if result, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&window))); result == 0 {
		procClipCursor.Call(0)
		return
	}
	if !windowFillsMonitor(window, monitor.Bounds) {
		procClipCursor.Call(0)
		return
	}
	taskbar, ok := autoHideTaskbarRect(monitor.Bounds)
	if !ok {
		procClipCursor.Call(0)
		return
	}
	virtual, ok := virtualScreenRect()
	if !ok {
		procClipCursor.Call(0)
		return
	}
	clip, keep := fullscreenCursorClip(true, virtual, monitor.Bounds, taskbar)
	if !keep {
		procClipCursor.Call(0)
		return
	}
	procClipCursor.Call(uintptr(unsafe.Pointer(&clip)))
}
