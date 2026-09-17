//go:build windows

package main

import (
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// Direct file drops onto the running VM window. The QEMU window is not ours, so
// accept drops with DragAcceptFiles and subclass it to see WM_DROPFILES. The
// drop point arrives in client coordinates; it is scaled to the guest display
// and handed to the same transfer path as the transfer window, with the point
// attached so the guest can deliver the files into the window under the cursor.

var vmDropSize atomic.Uint64 // width<<32 | height of the current guest display

func setVMDisplaySize(width, height int) {
	if width > 0 && height > 0 {
		vmDropSize.Store(uint64(uint32(width))<<32 | uint64(uint32(height)))
	}
}

var vmDropWindows sync.Map
var vmDropCallback = syscall.NewCallback(vmDropWindowProc)

const vmDropSubclassID = 0x4f44 // "OD"

func enableVMWindowDrops(hwnd uintptr) {
	if hwnd == 0 {
		return
	}
	if _, loaded := vmDropWindows.LoadOrStore(hwnd, true); loaded {
		return
	}
	shell32.NewProc("DragAcceptFiles").Call(hwnd, 1)
	comctl32.NewProc("SetWindowSubclass").Call(hwnd, vmDropCallback, vmDropSubclassID, 0)
	logf("drops: accepting file drops on the VM window %#x", hwnd)
}

func vmDropWindowProc(hwnd, message, w, l, id, data uintptr) uintptr {
	switch message {
	case 0x233: // WM_DROPFILES
		handleVMDrop(hwnd, w)
		return 0
	case 0x0002: // WM_DESTROY
		vmDropWindows.Delete(hwnd)
	}
	result, _, _ := comctl32.NewProc("DefSubclassProc").Call(hwnd, message, w, l)
	return result
}

func handleVMDrop(hwnd, drop uintptr) {
	defer shell32.NewProc("DragFinish").Call(drop)
	query := shell32.NewProc("DragQueryFileW")
	count, _, _ := query.Call(drop, 0xffffffff, 0, 0)
	if count == 0 || count > 1000 {
		return
	}
	paths := make([]string, 0, count)
	for i := uintptr(0); i < count; i++ {
		length, _, _ := query.Call(drop, i, 0, 0)
		if length == 0 || length > 32768 {
			return
		}
		buffer := make([]uint16, length+1)
		query.Call(drop, i, uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer)))
		paths = append(paths, syscall.UTF16ToString(buffer))
	}

	var rect struct{ left, top, right, bottom int32 }
	user32.NewProc("GetClientRect").Call(hwnd, uintptr(unsafe.Pointer(&rect)))
	width, height := int(rect.right-rect.left), int(rect.bottom-rect.top)
	var point []int
	if client, ok := dragQueryPoint(drop); ok && width > 0 && height > 0 {
		size := vmDropSize.Load()
		guestWidth, guestHeight := int(uint32(size>>32)), int(uint32(size))
		if guestWidth > 0 && guestHeight > 0 {
			point = []int{client[0] * guestWidth / width, client[1] * guestHeight / height}
		}
	}
	if err := sendDroppedFilesAt(paths, point); err != nil {
		logf("drops: delivering %d path(s) at %v failed: %v", len(paths), point, err)
		infoBox("These files could not be copied to Omarchy.\n\n" + err.Error())
		return
	}
	logf("drops: delivered %d path(s) at %v", len(paths), point)
}

func dragQueryPoint(drop uintptr) ([2]int, bool) {
	var point struct{ x, y int32 }
	ok, _, _ := shell32.NewProc("DragQueryPoint").Call(drop, uintptr(unsafe.Pointer(&point)))
	if ok == 0 {
		return [2]int{}, false
	}
	return [2]int{int(point.x), int(point.y)}, true
}
