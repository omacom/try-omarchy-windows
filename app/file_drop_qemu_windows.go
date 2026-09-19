//go:build windows

package main

import (
	"unsafe"
)

// Direct file drops on the VM window arrive as a QMP DISPLAY_FILE_DROP event
// (see file_drop.go). QEMU owns the SDL window and runs in another process, so
// the launcher cannot subclass that window to see WM_DROPFILES: SetWindowSubclass
// rewrites GWLP_WNDPROC, and a window procedure pointer is only valid inside the
// process that owns it. The WINQ-EMU runtime reports the drop instead, which is
// the only supported path.
//
// The event may carry the SDL drop point in display-window client coordinates;
// when it does not, the cursor is still over the release point while QEMU
// reports the completed drop, so that is mapped instead.

var (
	procGetClientRect  = user32.NewProc("GetClientRect")
	procScreenToClient = user32.NewProc("ScreenToClient")
)

// guestDropPoint includes the current display-window size so the guest can
// map the point using its current resolution and scale. point is nil when the runtime did not report coordinates.
func guestDropPoint(point *[2]int) []int {
	hwnd := qemuHwnd.Load()
	if hwnd == 0 {
		return nil
	}
	var rect struct{ left, top, right, bottom int32 }
	procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&rect)))
	width, height := int(rect.right-rect.left), int(rect.bottom-rect.top)
	if width <= 0 || height <= 0 {
		return nil
	}
	x, y := 0, 0
	if point != nil {
		x, y = point[0], point[1]
	} else {
		var cursor struct{ x, y int32 }
		procGetCursorPos.Call(uintptr(unsafe.Pointer(&cursor)))
		if ok, _, _ := procScreenToClient.Call(hwnd, uintptr(unsafe.Pointer(&cursor))); ok == 0 {
			return nil
		}
		x, y = int(cursor.x), int(cursor.y)
	}
	if x < 0 || y < 0 || x >= width || y >= height {
		return nil
	}
	return []int{x, y, width, height}
}
