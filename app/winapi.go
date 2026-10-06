//go:build windows

package main

import (
	"bytes"
	"io"
	"os"
	"runtime"
	"slices"
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
	procGetAsyncKeyState         = user32.NewProc("GetAsyncKeyState")
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
	mbIconError      = 0x10
	whKeyboardLL     = 13
	wmKeydown        = 0x100
	wmKeyup          = 0x101
	wmSyskeydown     = 0x104
	wmSyskeyup       = 0x105
	vkTab            = 0x09
	vkControl        = 0x11
	vkMenu           = 0x12
	vkEnd            = 0x23
	vkLwin           = 0x5B
	vkRwin           = 0x5C
	vkSnapshot       = 0x2C
	qsAllinput       = 0x04FF
	pmRemove         = 1
	cfUnicodetext    = 13
	cfDib            = 8
	cfDibV5          = 17
	gmemMoveable     = 2
	fsctlSetSparse   = 0x900C4
	fsctlSetZeroData = 0x980C8
	maxTitle         = 256
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

// foregroundPid is the process of the foreground window. A guest window that
// is still invisible while Omarchy boots does not count as the VM having focus.
func foregroundPid() uint32 {
	if curtainUp.Load() {
		return 0
	}
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

// The display enforcer finds QEMU's visible display windows and keeps them
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
// variables; only runTitleEnforcer's goroutine calls enforceDisplayWindows, so the
// handoff needs no locking.
type displayWindowState struct {
	index          int
	last           *windowPlacement // the placement remembered for the window
	target         *windowPlacement // where the window belongs (see nextPlacementStep)
	themeSet       bool             // the title bar theme below was applied or refused
	darkTitle      bool
	topologyRepair bool // retained while minimized or being dragged
}

func (state *displayWindowState) repairTopology(now *windowPlacement, dragging bool, monitors []hostMonitor, apply func(*windowPlacement) bool) *windowPlacement {
	if !state.topologyRepair || now == nil || dragging {
		return nil
	}
	restored := repairDisplayPlacement(now, state.index, monitors)
	if restored == nil || !restored.sameAs(now) && !apply(restored) {
		return nil
	}
	state.topologyRepair = false
	state.target = restored
	return restored
}

var (
	enumTitlePid               uint32
	enumTitleIcon              uintptr
	enumTitleDir               string
	enumTitleFullscreen        bool
	enumTitleFullscreenDisplay string
	enumTitleWindows           = map[uintptr]*displayWindowState{}
	enumTitleSeen              = map[uintptr]bool{}
	enumTitleMonitors          []screenRect
	enumTitleMonitorDetails    []hostMonitor
	enumTitleTopologyChanged   bool
	enumTitleDark              bool
	enumTitleCallback          = syscall.NewCallback(enumTitleProc)
	procGetClassNameW          = user32.NewProc("GetClassNameW")
)

func isQemuDisplayWindow(hwnd uintptr, pid uint32) bool {
	var owner uint32
	procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&owner)))
	if pid == 0 || owner != pid {
		return false
	}
	var class [64]uint16
	procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&class[0])), uintptr(len(class)))
	return syscall.UTF16ToString(class[:]) == "SDL_app"
}

func enumTitleProc(hwnd, _ uintptr) uintptr {
	if !isQemuDisplayWindow(hwnd, enumTitlePid) {
		return 1
	}
	if visible, _, _ := procIsWindowVisible.Call(hwnd); visible == 0 {
		return 1
	}
	enumTitleSeen[hwnd] = true
	var buf [maxTitle]uint16
	procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), maxTitle)
	title := syscall.UTF16ToString(buf[:])
	state, known := enumTitleWindows[hwnd]
	index, fromTitle := displayIndexFromTitle(title)
	parsed := fromTitle
	if !parsed {
		// The title hook may have renamed it before this saw QEMU's title.
		index, parsed = recordedDisplayIndex(hwnd)
	}
	grabbed := recordedDisplayGrab(hwnd)
	if fromTitle {
		grabbed = displayTitleGrabbed(title)
	}
	if !known || parsed && state.index != index {
		if !parsed {
			return 1
		}
		state = &displayWindowState{index: index}
		enumTitleWindows[hwnd] = state
		if !enumTitleFullscreen {
			placement := displayPlacement(enumTitleDir, index, enumTitleMonitorDetails)
			if placement == nil || !applyPlacement(hwnd, placement) {
				procShowWindow.Call(hwnd, swShowMaximized)
			}
			state.last, state.target = placement, placement
		}
		if enumTitleFullscreen {
			monitors := enumTitleMonitorDetails
			if len(monitors) > 0 {
				first, _ := selectedHostMonitor(enumTitleFullscreenDisplay, monitors)
				m := monitors[(first+index)%len(monitors)].Bounds
				procSetWindowPos.Call(hwnd, 0, uintptr(m.Left), uintptr(m.Top), uintptr(m.width()), uintptr(m.height()), 0x0004|0x0010)
			}
		}
		setTaskbarIdentity(hwnd)
	}
	retitledDisplays.Store(hwnd, recordedDisplay{enumTitlePid, state.index, grabbed})
	if curtainUp.Load() {
		concealForCurtain(hwnd)
		curtainTaskbar(hwnd)
	} else {
		uiDone()
	}
	if enumTitleIcon != 0 {
		procSendMessageW.Call(hwnd, 0x80, 1, enumTitleIcon)
		procSendMessageW.Call(hwnd, 0x80, 0, enumTitleIcon)
	}
	if wanted := displayWindowTitle(state.index); title != wanted {
		value, _ := syscall.UTF16PtrFromString(wanted)
		procSetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(value)))
	}
	if !state.themeSet || state.darkTitle != enumTitleDark {
		if !setDarkTitleBar(hwnd, enumTitleDark) && !state.themeSet {
			logf("title bar: Windows refused the dark mode attribute")
		}
		state.themeSet, state.darkTitle = true, enumTitleDark
	}
	if enumTitleTopologyChanged {
		state.topologyRepair = true
	}
	if !enumTitleFullscreen {
		if restored := state.repairTopology(capturePlacement(hwnd), beingDragged(hwnd), enumTitleMonitorDetails, func(p *windowPlacement) bool {
			return correctPlacement(hwnd, p)
		}); restored != nil {
			restored.SavedAt = time.Now()
			if saveDisplayPlacement(enumTitleDir, state.index, *restored) == nil {
				state.last = restored
			}
		}
	}
	if state.topologyRepair && enumTitleFullscreen && len(enumTitleMonitors) > 0 && capturePlacement(hwnd) != nil {
		var bounds screenRect
		if result, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&bounds))); result != 0 {
			if current := (&windowPlacement{Normal: bounds}); !current.usable(enumTitleMonitors) {
				first, _ := selectedHostMonitor(enumTitleFullscreenDisplay, enumTitleMonitorDetails)
				m := enumTitleMonitorDetails[(first+state.index)%len(enumTitleMonitorDetails)].Bounds
				if ok, _, _ := procSetWindowPos.Call(hwnd, 0, uintptr(m.Left), uintptr(m.Top), uintptr(m.width()), uintptr(m.height()), 0x0004|0x0010); ok == 0 {
					return 1
				}
			}
			state.topologyRepair = false
		}
	}
	if !enumTitleFullscreen {
		now := capturePlacement(hwnd)
		switch nextPlacementStep(now, state.last, state.target, takeUserMoved(hwnd), beingDragged(hwnd), !guestFollowsWindow.Load()) {
		case placementSave:
			now.SavedAt = time.Now()
			if saveDisplayPlacement(enumTitleDir, state.index, *now) == nil {
				state.last = now
			}
			state.target = now
		case placementRestore:
			correctPlacement(hwnd, state.target)
		}
	}
	return 1
}

func enforceDisplayWindows(pid uint32, dir string, fullscreen bool, fullscreenDisplay string, icon uintptr) {
	if pid != enumTitlePid {
		enumTitleWindows = map[uintptr]*displayWindowState{}
		enumTitleMonitors = nil
		enumTitleMonitorDetails = nil
	}
	pruneWindowEventState(pid, isQemuDisplayWindow)
	destroyedWindows.Range(func(key, _ any) bool {
		delete(enumTitleWindows, key.(uintptr))
		destroyedWindows.Delete(key)
		return true
	})
	enumTitlePid, enumTitleDir, enumTitleFullscreen, enumTitleFullscreenDisplay, enumTitleIcon = pid, dir, fullscreen, fullscreenDisplay, icon
	enumTitleSeen = map[uintptr]bool{}
	details := hostMonitors()
	monitors := make([]screenRect, 0, len(details))
	for _, monitor := range details {
		monitors = append(monitors, monitor.Bounds)
	}
	enumTitleTopologyChanged = !slices.Equal(enumTitleMonitors, monitors) || !slices.Equal(enumTitleMonitorDetails, details)
	enumTitleMonitors = monitors
	enumTitleMonitorDetails = details
	enumTitleDark = windowsAppsUseDarkTheme()
	procEnumWindows.Call(enumTitleCallback, 0)
	foreground, _, _ := procGetForegroundWindow.Call()
	selected := uintptr(0)
	for hwnd, state := range enumTitleWindows {
		if !enumTitleSeen[hwnd] {
			delete(enumTitleWindows, hwnd)
			continue
		}
		if selected == 0 || state.index == 0 {
			selected = hwnd
		}
	}
	if _, ok := enumTitleWindows[foreground]; ok {
		selected = foreground
	}
	qemuHwnd.Store(selected)
	primary := uintptr(0)
	for hwnd, state := range enumTitleWindows {
		if state.index == 0 {
			primary = hwnd
		}
	}
	curtainTick(primary)
}

func clipboardGetText() (string, bool) {
	text, status := clipboardReadText()
	return text, status == clipboardReady
}

func clipboardReadText() (string, clipboardReadStatus) {
	if r, _, _ := procIsClipboardFormatAvail.Call(cfUnicodetext); r == 0 {
		return "", clipboardUnsupported
	}
	if !openClipboard() {
		return "", clipboardRetry
	}
	defer procCloseClipboard.Call()
	h, _, _ := procGetClipboardData.Call(cfUnicodetext)
	if h == 0 {
		return "", clipboardRetry
	}
	size, _, _ := procGlobalSize.Call(h)
	if size < 2 || size > uintptr((maxClipboardTextBytes+1)*2) {
		return "", clipboardRejected
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		return "", clipboardRetry
	}
	defer procGlobalUnlock.Call(h)
	maxChars := int(size / 2)
	chars := make([]uint16, 0, maxChars)
	for i := 0; i < maxChars; i++ {
		c := *(*uint16)(unsafe.Pointer(p + uintptr(i)*2))
		if c == 0 {
			text := syscall.UTF16ToString(chars)
			if !clipboardTextAllowed(text) {
				return "", clipboardRejected
			}
			return text, clipboardReady
		}
		chars = append(chars, c)
	}
	// CF_UNICODETEXT is required to be NUL-terminated. Refuse a malformed
	// clipboard handle instead of reading beyond its allocation.
	return "", clipboardRejected
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
	item, status := clipboardReadItem()
	return item, status == clipboardReady
}
func clipboardReadItem() (clipItem, clipboardReadStatus) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if r, _, _ := procIsClipboardFormatAvail.Call(cfHDrop); r != 0 {
		item, status := clipboardReadFiles()
		if status != clipboardUnsupported {
			return item, status
		}
	}
	return clipboardReadNonFileItem()
}

func clipboardReadNonFileItem() (clipItem, clipboardReadStatus) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if text, status := clipboardReadText(); status != clipboardUnsupported {
		return textItem(text), status
	}
	hasPNG, _, _ := procIsClipboardFormatAvail.Call(pngClipboardFormat)
	hasDIB, _, _ := procIsClipboardFormatAvail.Call(cfDib)
	hasDIBV5, _, _ := procIsClipboardFormatAvail.Call(cfDibV5)
	if hasPNG == 0 && hasDIB == 0 && hasDIBV5 == 0 {
		return clipItem{}, clipboardUnsupported
	}
	if !openClipboard() {
		return clipItem{}, clipboardRetry
	}
	defer procCloseClipboard.Call()
	if hasPNG != 0 {
		data, status := clipboardGlobalBytesStatus(pngClipboardFormat, maxClipboardImageBytes)
		if status == clipboardRetry {
			return clipItem{}, status
		}
		if status == clipboardReady {
			item := pngItem(data)
			if item.allowed() {
				return item, clipboardReady
			}
		}
	}
	format := uintptr(cfDib)
	if hasDIBV5 != 0 {
		format = cfDibV5
	} else if hasDIB == 0 {
		return clipItem{}, clipboardRejected
	}
	dib, status := clipboardGlobalBytesStatus(format, maxClipboardDIBBytes)
	if status != clipboardReady {
		return clipItem{}, status
	}
	data, err := dibToPNG(dib)
	if err != nil {
		return clipItem{}, clipboardRejected
	}
	item := pngItem(data)
	if !item.allowed() {
		return clipItem{}, clipboardRejected
	}
	return item, clipboardReady
}

// clipboardGlobalBytes copies a clipboard handle's memory; the clipboard
// must already be open.
func clipboardGlobalBytes(format uintptr, limit int) ([]byte, bool) {
	data, status := clipboardGlobalBytesStatus(format, limit)
	return data, status == clipboardReady
}
func clipboardGlobalBytesStatus(format uintptr, limit int) ([]byte, clipboardReadStatus) {
	h, _, _ := procGetClipboardData.Call(format)
	if h == 0 {
		return nil, clipboardRetry
	}
	size, _, _ := procGlobalSize.Call(h)
	if size == 0 {
		return nil, clipboardUnsupported
	}
	if size > uintptr(limit) {
		return nil, clipboardRejected
	}
	p, _, _ := procGlobalLock.Call(h)
	if p == 0 {
		return nil, clipboardRetry
	}
	defer procGlobalUnlock.Call(h)
	return append([]byte(nil), unsafe.Slice((*byte)(unsafe.Pointer(p)), int(size))...), clipboardReady
}

func clipboardSetItem(item clipItem) bool {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if item.Kind == clipFiles {
		return clipboardSetFiles(item)
	}
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

// punchHole deallocates a zero range of a sparse file so it stops costing
// space on the Windows drive. Reads of the range still return zeros.
func punchHole(f *os.File, offset, length int64) error {
	var zeroData struct{ fileOffset, beyondFinalZero int64 }
	zeroData.fileOffset, zeroData.beyondFinalZero = offset, offset+length
	var returned uint32
	r, _, err := procDeviceIoControl.Call(f.Fd(), fsctlSetZeroData, uintptr(unsafe.Pointer(&zeroData)), unsafe.Sizeof(zeroData), 0, 0,
		uintptr(unsafe.Pointer(&returned)), 0)
	if r == 0 {
		return err
	}
	return nil
}
