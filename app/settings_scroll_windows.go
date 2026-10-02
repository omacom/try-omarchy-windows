//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var (
	procSetScrollInfo = user32.NewProc("SetScrollInfo")
	procGetScrollInfo = user32.NewProc("GetScrollInfo")
	procGetFocus      = user32.NewProc("GetFocus")
	procDeleteObject  = syscall.NewLazyDLL("gdi32.dll").NewProc("DeleteObject")
)

type settingsScrollControl struct {
	handle     uintptr
	x, y, w, h int32
}
type settingsScroll struct {
	window, viewport             uintptr
	top, height, content, offset int32
	controls                     []settingsScrollControl
}
type settingsScrollInfo struct {
	size, mask uint32
	min, max   int32
	page       uint32
	pos, track int32
}

func (s *settingsScroll) move(offset int32) {
	maximum := s.content - s.top - s.height
	if maximum < 0 {
		maximum = 0
	}
	if offset < 0 {
		offset = 0
	}
	if offset > maximum {
		offset = maximum
	}
	s.offset = offset
	// Move the whole page before painting. Sequential moves can otherwise copy
	// pixels from overlapping old control positions, especially under DPI
	// virtualization. The viewport clips children out of its background erase.
	const moveFlags = 0x0004 | 0x0010 | 0x0008 | 0x0100 // NOZORDER | NOACTIVATE | NOREDRAW | NOCOPYBITS
	for _, c := range s.controls {
		y := c.y - s.top - offset
		procSetWindowPos.Call(c.handle, 0, uintptr(c.x), uintptr(y), uintptr(c.w), uintptr(c.h), moveFlags)
	}
	info := settingsScrollInfo{mask: 0x7, max: max(int32(0), s.content-s.top-1), page: uint32(s.height), pos: offset}
	info.size = uint32(unsafe.Sizeof(info))
	procSetScrollInfo.Call(s.window, 1, uintptr(unsafe.Pointer(&info)), 1)
	// Erase and repaint every child at its final position, including transparent
	// labels; invalidating only the WS_CLIPCHILDREN viewport leaves them stale.
	const repaintFlags = 0x0001 | 0x0004 | 0x0080 | 0x0100 // INVALIDATE | ERASE | ALLCHILDREN | UPDATENOW
	procRedrawWindow.Call(s.viewport, 0, 0, repaintFlags)
}

func (s *settingsScroll) handle(message, wParam uintptr) bool {
	switch message {
	case 0x0115: // WM_VSCROLL
		next := s.offset
		switch wParam & 0xffff {
		case 0:
			next -= 24
		case 1:
			next += 24
		case 2:
			next -= s.height - 24
		case 3:
			next += s.height - 24
		case 4, 5:
			info := settingsScrollInfo{mask: 0x10}
			info.size = uint32(unsafe.Sizeof(info))
			procGetScrollInfo.Call(s.window, 1, uintptr(unsafe.Pointer(&info)))
			next = info.track
		case 6:
			next = 0
		case 7:
			next = s.content
		}
		s.move(next)
		return true
	case 0x020A: // WM_MOUSEWHEEL
		s.move(s.offset - int32(int16(wParam>>16))/120*72)
		return true
	}
	return false
}

func (s *settingsScroll) revealFocus() {
	focus, _, _ := procGetFocus.Call()
	for _, c := range s.controls {
		if c.handle != focus {
			continue
		}
		if c.y < s.offset+s.top {
			s.move(c.y - s.top)
		} else if c.y+c.h > s.offset+s.top+s.height {
			s.move(c.y + c.h - s.top - s.height)
		}
		return
	}
}

// A native child viewport clips page controls without hiding them from Tab
// navigation. The fixed navigation and actions remain children of the frame.
const settingsViewportID = 2099

func createSettingsViewport(parent, instance uintptr) uintptr {
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettingsViewport")
	callback := syscall.NewCallback(func(h, message, w, l uintptr) uintptr {
		switch message {
		case wmCommand, 0x002b, 0x0133, 0x0134, 0x0135, wmCtlcolorstatic, 0x020a:
			result, _, _ := procSendMessageW.Call(parent, message, w, l)
			return result
		case 0x0014: // WM_ERASEBKGND uses the parent palette.
			brush, _, _ := procSendMessageW.Call(parent, wmCtlcolorstatic, w, h)
			var rect [4]int32
			procGetClientRect.Call(h, uintptr(unsafe.Pointer(&rect)))
			procFillRect.Call(w, uintptr(unsafe.Pointer(&rect)), brush)
			return 1
		}
		result, _, _ := procDefWindowProcW.Call(h, message, w, l)
		return result
	})
	type windowClass struct {
		size, style                   uint32
		callback                      uintptr
		classExtra, windowExtra       int32
		instance, icon, cursor, brush uintptr
		menu, name                    *uint16
		smallIcon                     uintptr
	}
	wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: callback, instance: instance, brush: colorBtnface + 1, name: class}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("settings viewport: %v", err)
		return 0
	}
	// WS_EX_CONTROLPARENT lets native dialog navigation enter the viewport.
	viewport, _, err := procCreateWindowExW.Call(0x00010000, uintptr(unsafe.Pointer(class)), 0, wsChild|wsVisible|0x02000000, 0, 0, 1, 1, parent, settingsViewportID, instance, 0)
	if viewport == 0 {
		logf("settings viewport: %v", err)
	}
	return viewport
}
