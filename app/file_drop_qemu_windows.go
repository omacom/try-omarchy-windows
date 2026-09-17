//go:build windows

package main

import (
	"sync/atomic"
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

var vmDropSize atomic.Uint64 // width<<32 | height of the current guest display

var (
	procGetClientRect  = user32.NewProc("GetClientRect")
	procScreenToClient = user32.NewProc("ScreenToClient")
)

func setVMDisplaySize(width, height int) {
	if width > 0 && height > 0 {
		vmDropSize.Store(uint64(uint32(width))<<32 | uint64(uint32(height)))
	}
}

// guestDropPoint scales a drop point from display-window client coordinates to
// the guest display. point is nil when the runtime did not report coordinates.
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
	size := vmDropSize.Load()
	guestWidth, guestHeight := int(uint32(size>>32)), int(uint32(size))
	if guestWidth <= 0 || guestHeight <= 0 {
		return nil
	}
	return []int{x * guestWidth / width, y * guestHeight / height}
}
