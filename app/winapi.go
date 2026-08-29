//go:build windows

package main

import (
	"bytes"
	"io"
	"os"
	"syscall"
	"unsafe"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")

	procMessageBoxW              = user32.NewProc("MessageBoxW")
	procSetWindowsHookExW        = user32.NewProc("SetWindowsHookExW")
	procUnhookWindowsHookEx      = user32.NewProc("UnhookWindowsHookEx")
	procCallNextHookEx           = user32.NewProc("CallNextHookEx")
	procGetForegroundWindow      = user32.NewProc("GetForegroundWindow")
	procGetWindowThreadProcessId = user32.NewProc("GetWindowThreadProcessId")
	procMsgWaitForMultipleObj    = user32.NewProc("MsgWaitForMultipleObjects")
	procPeekMessageW             = user32.NewProc("PeekMessageW")
	procEnumWindows              = user32.NewProc("EnumWindows")
	procIsWindowVisible          = user32.NewProc("IsWindowVisible")
	procGetWindowTextW           = user32.NewProc("GetWindowTextW")
	procSetWindowTextW           = user32.NewProc("SetWindowTextW")
	procOpenClipboard            = user32.NewProc("OpenClipboard")
	procCloseClipboard           = user32.NewProc("CloseClipboard")
	procEmptyClipboard           = user32.NewProc("EmptyClipboard")
	procGetClipboardData         = user32.NewProc("GetClipboardData")
	procSetClipboardData         = user32.NewProc("SetClipboardData")
	procIsClipboardFormatAvail   = user32.NewProc("IsClipboardFormatAvailable")
	procGetClipboardSeqNum       = user32.NewProc("GetClipboardSequenceNumber")
	procSystemParametersInfoW    = user32.NewProc("SystemParametersInfoW")
	procGlobalAlloc              = kernel32.NewProc("GlobalAlloc")
	procGlobalLock               = kernel32.NewProc("GlobalLock")
	procGlobalUnlock             = kernel32.NewProc("GlobalUnlock")
	procGlobalFree               = kernel32.NewProc("GlobalFree")
	procDeviceIoControl          = kernel32.NewProc("DeviceIoControl")
)

const (
	mbIconError     = 0x10
	whKeyboardLL    = 13
	wmKeydown       = 0x100
	wmSyskeydown    = 0x104
	vkLwin          = 0x5B
	vkRwin          = 0x5C
	qsAllinput      = 0x04FF
	pmRemove        = 1
	cfUnicodetext   = 13
	gmemMoveable    = 2
	fsctlSetSparse  = 0x900C4
	maxTitle        = 256
)

type msgStruct struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	ptX     int32
	ptY     int32
}

func errorBox(text string) {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(appTitle)
	procMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), mbIconError)
}

func setSparse(f *os.File) error {
	var returned uint32
	r, _, err := procDeviceIoControl.Call(f.Fd(), fsctlSetSparse, 0, 0, 0, 0,
		uintptr(unsafe.Pointer(&returned)), 0)
	if r == 0 {
		return err
	}
	return nil
}

// sparseCopy skips all-zero 1 MiB blocks (the file is already marked sparse,
// so seeking past them leaves holes and the factory image lands at its real
// data size instead of a full 6 GiB).
func sparseCopy(dst *os.File, src *os.File, total int64, ui *progressUI) error {
	buf := make([]byte, 1<<20)
	zero := make([]byte, 1<<20)
	var off int64
	for {
		n, err := io.ReadFull(src, buf)
		if n > 0 {
			if bytes.Equal(buf[:n], zero[:n]) {
				off += int64(n)
			} else {
				if _, werr := dst.WriteAt(buf[:n], off); werr != nil {
					return werr
				}
				off += int64(n)
			}
			if ui != nil {
				ui.setProgress(off, total)
			}
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return dst.Truncate(off)
		}
		if err != nil {
			return err
		}
	}
}

// screenSize returns the primary screen bounds (fullscreen) or the desktop
// work area minus window chrome (windowed) - the guest console is sized to
// match so the picture fills the window from the first frame (launch-UX
// contract in NOTES.md).
func screenSize(fullscreen bool) (int, int) {
	if fullscreen {
		w, _, _ := procGetSystemMetrics.Call(smCxscreen)
		h, _, _ := procGetSystemMetrics.Call(smCyscreen)
		return int(w), int(h)
	}
	var r struct{ left, top, right, bottom int32 }
	const spiGetworkarea = 0x30
	if ret, _, _ := procSystemParametersInfoW.Call(spiGetworkarea, 0, uintptr(unsafe.Pointer(&r)), 0); ret == 0 {
		return 1280, 800
	}
	return int(r.right - r.left), int(r.bottom-r.top) - 31 // minus title bar
}

func foregroundPid() uint32 {
	hwnd, _, _ := procGetForegroundWindow.Call()
	if hwnd == 0 {
		return 0
	}
	var pid uint32
	procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
	return pid
}

// enforceTitle finds the QEMU process's visible top-level window, keeps it
// titled appTitle (QEMU rewrites its own title on every grab toggle, so the
// caller reasserts this periodically), maximizes it the first time it appears
// (launch-UX contract: maximized by default, never fullscreen, never a small
// floating window) and keeps our icon on it (HICONs are USER handles, valid
// across processes in a session, so WM_SETICON onto QEMU's window works).
// Users must never see QEMU chrome.
func enforceTitle(pid uint32, maximize *bool, appIcon uintptr) {
	cb := syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		var wpid uint32
		procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&wpid)))
		if wpid != pid {
			return 1
		}
		if v, _, _ := procIsWindowVisible.Call(hwnd); v == 0 {
			return 1
		}
		if *maximize {
			*maximize = false
			const swMaximize = 3
			procShowWindow.Call(hwnd, swMaximize)
		}
		if appIcon != 0 {
			const wmSeticonMsg = 0x80
			procSendMessageW.Call(hwnd, wmSeticonMsg, 1, appIcon) // ICON_BIG
			procSendMessageW.Call(hwnd, wmSeticonMsg, 0, appIcon) // ICON_SMALL
		}
		buf := make([]uint16, maxTitle)
		procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), maxTitle)
		if syscall.UTF16ToString(buf) != appTitle {
			t, _ := syscall.UTF16PtrFromString(appTitle)
			procSetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(t)))
		}
		return 1
	})
	procEnumWindows.Call(cb, 0)
}

func clipboardSeq() uint32 {
	r, _, _ := procGetClipboardSeqNum.Call()
	return uint32(r)
}

func clipboardGetText() (string, bool) {
	if r, _, _ := procIsClipboardFormatAvail.Call(cfUnicodetext); r == 0 {
		return "", false
	}
	if r, _, _ := procOpenClipboard.Call(0); r == 0 {
		return "", false
	}
	defer procCloseClipboard.Call()
	h, _, _ := procGetClipboardData.Call(cfUnicodetext)
	if h == 0 {
		return "", false
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		return "", false
	}
	defer procGlobalUnlock.Call(h)
	var chars []uint16
	for i := 0; ; i++ {
		c := *(*uint16)(unsafe.Pointer(p + uintptr(i)*2))
		if c == 0 {
			break
		}
		chars = append(chars, c)
	}
	return syscall.UTF16ToString(chars), true
}

func clipboardSetText(s string) bool {
	u, err := syscall.UTF16FromString(s)
	if err != nil {
		return false
	}
	size := uintptr(len(u) * 2)
	h, _, _ := procGlobalAlloc.Call(gmemMoveable, size)
	if h == 0 {
		return false
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		procGlobalFree.Call(h)
		return false
	}
	dst := unsafe.Slice((*uint16)(unsafe.Pointer(p)), len(u))
	copy(dst, u)
	procGlobalUnlock.Call(h)
	if r, _, _ := procOpenClipboard.Call(0); r == 0 {
		procGlobalFree.Call(h)
		return false
	}
	defer procCloseClipboard.Call()
	procEmptyClipboard.Call()
	if r, _, _ := procSetClipboardData.Call(cfUnicodetext, h); r == 0 {
		procGlobalFree.Call(h)
		return false
	}
	return true // the system owns the handle after SetClipboardData succeeds
}
