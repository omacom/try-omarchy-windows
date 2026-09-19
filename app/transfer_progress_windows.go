//go:build windows

package main

import (
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

type transferWindow struct {
	progress *transferProgress
	status   uintptr
}

var transferWindows sync.Map
var transferClassOnce sync.Once
var transferClassOK bool
var transferWindowCallback = syscall.NewCallback(transferWindowProc)

func transferWindowProc(hwnd, message, w, l uintptr) uintptr {
	if value, ok := transferWindows.Load(hwnd); ok {
		state := value.(*transferWindow)
		switch message {
		case 0x113: // WM_TIMER, UI-thread polling avoids posts to reused handles.
			select {
			case <-state.progress.done:
				procDestroyWindow.Call(hwnd)
				return 0
			default:
			}
			usbSetText(state.status, state.progress.text.Load().(string))
			return 0
		case wmCommand:
			if w&0xffff == 2 {
				state.progress.cancel()
				procDestroyWindow.Call(hwnd)
				return 0
			}
		case wmClose:
			procDestroyWindow.Call(hwnd)
			return 0
		case wmDestroy:
			transferWindows.Delete(hwnd)
			procPostQuitMessage.Call(0)
			return 0
		}
	}
	result, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
	return result
}

func showTransferProgress(progress *transferProgress) {
	// Small clipboard copies complete without opening a window.
	select {
	case <-progress.done:
		return
	case <-time.After(2 * time.Second):
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	instance, _, _ := procGetModuleHandleW.Call(0)
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileTransfer")
	transferClassOnce.Do(func() {
		type windowClass struct {
			size, style                   uint32
			callback                      uintptr
			classExtra, windowExtra       int32
			instance, icon, cursor, brush uintptr
			menu, class                   *uint16
			smallIcon                     uintptr
		}
		cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
		wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: transferWindowCallback, instance: instance, cursor: cursor, brush: colorBtnface + 1, class: class}
		atom, _, _ := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))
		transferClassOK = atom != 0
	})
	if !transferClassOK {
		logf("could not create file transfer window")
		return
	}
	// WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW keeps background transfers from
	// taking focus. Closing dismisses progress; Cancel explicitly stops copying.
	title, _ := syscall.UTF16PtrFromString("Copying files")
	hwnd, _, _ := procCreateWindowExW.Call(0x08000080, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), wsCaption|wsSysmenu|wsVisible, 120, 120, 500, 150, 0, 0, instance, 0)
	if hwnd == 0 {
		return
	}
	state := &transferWindow{progress: progress}
	transferWindows.Store(hwnd, state)
	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	control := func(class, label string, x, y, width, height int, id, style uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		text, _ := syscall.UTF16PtrFromString(label)
		handle, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(text)), wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(width), uintptr(height), hwnd, id, instance, 0)
		procSendMessageW.Call(handle, wmSetfont, font, 1)
		return handle
	}
	state.status = control("STATIC", progress.text.Load().(string), 16, 12, 460, 44, 4500, ssNoprefix)
	button := control("BUTTON", "Cancel", 366, 64, 100, 28, 2, wsTabstop)
	if state.status == 0 || button == 0 {
		progress.cancel()
		procDestroyWindow.Call(hwnd)
	} else {
		procSetTimer.Call(hwnd, 1, 100, 0)
	}
	var message msgStruct
	for {
		result, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if result == 0 || int32(result) == -1 {
			break
		}
		if handled, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&message))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
}
