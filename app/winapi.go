//go:build windows

package main

import (
	"bytes"
	"io"
	"os"
	"syscall"
	"time"
	"unsafe"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	advapi32 = syscall.NewLazyDLL("advapi32.dll")

	procMessageBoxW              = user32.NewProc("MessageBoxW")
	procSetWindowsHookExW        = user32.NewProc("SetWindowsHookExW")
	procUnhookWindowsHookEx      = user32.NewProc("UnhookWindowsHookEx")
	procCallNextHookEx           = user32.NewProc("CallNextHookEx")
	procGetForegroundWindow      = user32.NewProc("GetForegroundWindow")
	procGetWindowThreadProcessId = user32.NewProc("GetWindowThreadProcessId")
	procClipCursor               = user32.NewProc("ClipCursor")
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
	procGetClipboardSequenceNum  = user32.NewProc("GetClipboardSequenceNumber")
	procRegisterClipboardFormatW = user32.NewProc("RegisterClipboardFormatW")
	procIsClipboardFormatAvail   = user32.NewProc("IsClipboardFormatAvailable")
	procSystemParametersInfoW    = user32.NewProc("SystemParametersInfoW")
	procGlobalAlloc              = kernel32.NewProc("GlobalAlloc")
	procGlobalSize               = kernel32.NewProc("GlobalSize")
	procGlobalLock               = kernel32.NewProc("GlobalLock")
	procGlobalUnlock             = kernel32.NewProc("GlobalUnlock")
	procGlobalFree               = kernel32.NewProc("GlobalFree")
	procDeviceIoControl          = kernel32.NewProc("DeviceIoControl")
)

const (
	mbIconError    = 0x10
	whKeyboardLL   = 13
	wmKeydown      = 0x100
	wmSyskeydown   = 0x104
	vkLwin         = 0x5B
	vkRwin         = 0x5C
	qsAllinput     = 0x04FF
	pmRemove       = 1
	cfUnicodetext  = 13
	cfDib          = 8
	cfDibV5        = 17
	gmemMoveable   = 2
	fsctlSetSparse = 0x900C4
	maxTitle       = 256
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

// mbFront forces a dialog above everything and steals focus - errors from a
// background phase were invisible behind other windows, which reads as "the
// app silently died" (it happened, pre-announcement).
const mbFront = 0x50000 // MB_TOPMOST | MB_SETFOREGROUND

func errorBox(text string) {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(appTitle)
	procMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), mbIconError|mbFront)
}

// availMemMiB returns total and available physical memory in MiB (0,0 if the
// query fails).
func availMemMiB() (int, int) {
	var ms struct {
		length, memoryLoad                                                    uint32
		totalPhys, availPhys, totalPage, availPage, totalVirt, availVirt, ext uint64
	}
	ms.length = uint32(unsafe.Sizeof(ms))
	proc := kernel32.NewProc("GlobalMemoryStatusEx")
	if r, _, _ := proc.Call(uintptr(unsafe.Pointer(&ms))); r == 0 {
		return 0, 0
	}
	return int(ms.totalPhys >> 20), int(ms.availPhys >> 20)
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
		if err := checkSetupCancelled(); err != nil {
			return err
		}
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

// releaseQemuCursor defeats SDL's automatic window confinement. QEMU has an
// absolute virtio tablet, so it never needs to trap the host pointer. This is
// especially important over RDP, where clicking the VM can otherwise make the
// Windows taskbar unreachable until Ctrl+Alt+G or the secure desktop breaks
// SDL's grab.
func releaseQemuCursor() {
	if pid := qemuPid.Load(); pid != 0 && foregroundPid() == pid {
		procClipCursor.Call(0)
	}
}

// enforceTitle finds the QEMU process's visible top-level window, keeps it
// titled appTitle (QEMU rewrites its own title on every grab toggle, so the
// caller reasserts this periodically), maximizes it the first time it appears
// (launch-UX contract: maximized by default, never fullscreen, never a small
// floating window) and keeps our icon on it (HICONs are USER handles, valid
// across processes in a session, so WM_SETICON onto QEMU's window works).
// Users must never see QEMU chrome.
//
// The EnumWindows callback is built once for the process. syscall.NewCallback
// permanently reserves one of a hard 2000-entry table and never frees it, and
// a capturing closure allocates a fresh funcval every call so it cannot be
// cached. Creating it here instead - at one call per second - killed the
// launcher with "too many callback functions" after ~33 minutes, taking the
// Windows-key hook, the close guard and the clipboard bridge down with it
// while QEMU kept running, which reads to the user as Windows shortcuts
// suddenly leaking through. The callback reads its inputs from the enumTitle*
// variables; only runTitleEnforcer's goroutine calls enforceTitle, so the
// handoff needs no locking.
var (
	enumTitlePid      uint32
	enumTitleMaximize *bool
	enumTitleIcon     uintptr
	enumTitleCallback = syscall.NewCallback(enumTitleProc)
	// enumTitleRestore is the remembered placement for this launch; nil keeps
	// the maximized default.
	enumTitleRestore *windowPlacement
)

func enumTitleProc(hwnd, _ uintptr) uintptr {
	var wpid uint32
	procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&wpid)))
	if wpid != enumTitlePid {
		return 1
	}
	if v, _, _ := procIsWindowVisible.Call(hwnd); v == 0 {
		return 1
	}
	qemuHwnd.Store(hwnd) // the close guard needs the live window handle
	uiDone()             // the VM window is on screen: the splash's job is over
	if *enumTitleMaximize {
		*enumTitleMaximize = false
		if enumTitleRestore == nil || !applyPlacement(hwnd, enumTitleRestore) {
			const swMaximize = 3
			procShowWindow.Call(hwnd, swMaximize)
		}
		setTaskbarIdentity(hwnd) // once per window: our taskbar icon + name
	}
	if enumTitleIcon != 0 {
		const wmSeticonMsg = 0x80
		procSendMessageW.Call(hwnd, wmSeticonMsg, 1, enumTitleIcon) // ICON_BIG
		procSendMessageW.Call(hwnd, wmSeticonMsg, 0, enumTitleIcon) // ICON_SMALL
	}
	buf := make([]uint16, maxTitle)
	procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), maxTitle)
	if syscall.UTF16ToString(buf) != appTitle {
		t, _ := syscall.UTF16PtrFromString(appTitle)
		procSetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(t)))
	}
	return 1
}

func enforceTitle(pid uint32, maximize *bool, appIcon uintptr, restore *windowPlacement) {
	enumTitlePid, enumTitleMaximize, enumTitleIcon, enumTitleRestore = pid, maximize, appIcon, restore
	procEnumWindows.Call(enumTitleCallback, 0)
}

func clipboardGetText() (string, bool) {
	if r, _, _ := procIsClipboardFormatAvail.Call(cfUnicodetext); r == 0 {
		return "", false
	}
	if !openClipboard() {
		return "", false
	}
	defer procCloseClipboard.Call()
	h, _, _ := procGetClipboardData.Call(cfUnicodetext)
	if h == 0 {
		return "", false
	}
	size, _, _ := procGlobalSize.Call(h)
	if size < 2 || size > uintptr((maxClipboardTextBytes+1)*2) {
		return "", false
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		return "", false
	}
	defer procGlobalUnlock.Call(h)
	maxChars := int(size / 2)
	chars := make([]uint16, 0, maxChars)
	for i := 0; i < maxChars; i++ {
		c := *(*uint16)(unsafe.Pointer(p + uintptr(i)*2))
		if c == 0 {
			text := syscall.UTF16ToString(chars)
			return text, clipboardTextAllowed(text)
		}
		chars = append(chars, c)
	}
	// CF_UNICODETEXT is required to be NUL-terminated. Refuse a malformed
	// clipboard handle instead of reading beyond its allocation.
	return "", false
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
	if !openClipboard() {
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

func openClipboard() bool {
	for attempt := 0; attempt < 20; attempt++ {
		if r, _, _ := procOpenClipboard.Call(0); r != 0 {
			return true
		}
		time.Sleep(25 * time.Millisecond)
	}
	return false
}

// pngClipboardFormat is the registered "PNG" format browsers and image
// editors use beside CF_DIB. Registering an existing name returns its id.
var pngClipboardFormat = func() uintptr {
	name, _ := syscall.UTF16PtrFromString("PNG")
	id, _, _ := procRegisterClipboardFormatW.Call(uintptr(unsafe.Pointer(name)))
	return id
}()

func clipboardSequence() uint32 {
	seq, _, _ := procGetClipboardSequenceNum.Call()
	return uint32(seq)
}

// clipboardGetItem reads the Windows clipboard as text when text is offered,
// otherwise as a PNG image from the registered PNG format or a DIB.
func clipboardGetItem() (clipItem, bool) {
	if text, ok := clipboardGetText(); ok {
		return textItem(text), true
	}
	if r, _, _ := procIsClipboardFormatAvail.Call(cfUnicodetext); r != 0 {
		return clipItem{}, false
	}
	hasPNG, _, _ := procIsClipboardFormatAvail.Call(pngClipboardFormat)
	hasDIB, _, _ := procIsClipboardFormatAvail.Call(cfDib)
	if hasPNG == 0 && hasDIB == 0 {
		return clipItem{}, false
	}
	if !openClipboard() {
		return clipItem{}, false
	}
	defer procCloseClipboard.Call()
	if hasPNG != 0 {
		if data, ok := clipboardGlobalBytes(pngClipboardFormat, maxClipboardImageBytes); ok {
			item := pngItem(data)
			return item, item.allowed()
		}
	}
	format := uintptr(cfDibV5)
	if r, _, _ := procIsClipboardFormatAvail.Call(cfDibV5); r == 0 {
		format = cfDib
	}
	dib, ok := clipboardGlobalBytes(format, 4*maxDIBSide*maxDIBSide+dibV5HeaderSize+1024)
	if !ok {
		return clipItem{}, false
	}
	data, err := dibToPNG(dib)
	if err != nil {
		return clipItem{}, false
	}
	item := pngItem(data)
	return item, item.allowed()
}

// clipboardGlobalBytes copies a clipboard handle's memory; the clipboard
// must already be open.
func clipboardGlobalBytes(format uintptr, limit int) ([]byte, bool) {
	h, _, _ := procGetClipboardData.Call(format)
	if h == 0 {
		return nil, false
	}
	size, _, _ := procGlobalSize.Call(h)
	if size == 0 || int(size) > limit {
		return nil, false
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		return nil, false
	}
	defer procGlobalUnlock.Call(h)
	return append([]byte(nil), unsafe.Slice((*byte)(unsafe.Pointer(p)), int(size))...), true
}

func clipboardSetItem(item clipItem) bool {
	if item.Kind == clipText {
		return clipboardSetText(string(item.Data))
	}
	dib, err := pngToDIB(item.Data)
	if err != nil {
		return false
	}
	hDib := globalCopy(dib)
	hPNG := globalCopy(item.Data)
	if hDib == 0 || hPNG == 0 {
		procGlobalFree.Call(hDib)
		procGlobalFree.Call(hPNG)
		return false
	}
	if !openClipboard() {
		procGlobalFree.Call(hDib)
		procGlobalFree.Call(hPNG)
		return false
	}
	defer procCloseClipboard.Call()
	procEmptyClipboard.Call()
	if r, _, _ := procSetClipboardData.Call(cfDib, hDib); r == 0 {
		procGlobalFree.Call(hDib)
		procGlobalFree.Call(hPNG)
		return false
	}
	if r, _, _ := procSetClipboardData.Call(pngClipboardFormat, hPNG); r == 0 {
		procGlobalFree.Call(hPNG)
	}
	return true
}

func globalCopy(data []byte) uintptr {
	h, _, _ := procGlobalAlloc.Call(gmemMoveable, uintptr(len(data)))
	if h == 0 {
		return 0
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		procGlobalFree.Call(h)
		return 0
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(p)), len(data)), data)
	procGlobalUnlock.Call(h)
	return h
}
