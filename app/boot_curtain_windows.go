//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Windows side of the boot curtain (boot_curtain.go). Hiding QEMU's window
// would let it be resized to whatever the guest's display is while nobody
// watches. Instead the window stays shown, in its real place and size, with
// an empty window region, so nothing of it is drawn or clickable, and no
// taskbar button. It is not made layered: on some AMD drivers an OpenGL
// window that is layered while QEMU sets up its display never reaches the
// screen again, even after the style is removed (#358). With an empty region
// its picture cannot be read either, so the curtain lifts on the session
// timers rather than on a drawn wallpaper.

var (
	procSetWindowRgn       = user32.NewProc("SetWindowRgn")
	procGetWindowLongW     = user32.NewProc("GetWindowLongW")
	procSetWindowLongW     = user32.NewProc("SetWindowLongW")
	procPrintWindow        = user32.NewProc("PrintWindow")
	procGetDC              = user32.NewProc("GetDC")
	procReleaseDC          = user32.NewProc("ReleaseDC")
	gdi32                  = syscall.NewLazyDLL("gdi32.dll")
	procCreateCompatibleDC = gdi32.NewProc("CreateCompatibleDC")
	procCreateDIBSection   = gdi32.NewProc("CreateDIBSection")
	procDeleteDC           = gdi32.NewProc("DeleteDC")
	procCreateRectRgn      = gdi32.NewProc("CreateRectRgn")
	procClientToScreen     = user32.NewProc("ClientToScreen")

	// curtainUp is true from launch until the window is shown for the first
	// time. Reboots after that show the guest's window as they always did.
	curtainUp      atomic.Bool
	curtainMu      sync.Mutex
	curtainWindows = map[uintptr]*curtainWindow{}
	// Windows the lifting curtain has not given their frame back yet.
	curtainLifting = map[uintptr]bool{}
	// The disk this launch boots and its folder, for recording that it
	// reached the desktop.
	curtainDiskID, curtainVMDir string
	// Consecutive frames that showed the desktop, and whether a capture is
	// still running (a busy QEMU can hold one up).
	curtainCapturing atomic.Bool
	curtainBoot      time.Time
	curtainStatus    string
	// When watchDesktop last looked at the window after the curtain lifted.
	desktopCheckedAt time.Time
)

type curtainWindow struct {
	// ready is set once the window is hidden. Until then the goroutine
	// hiding it owns it, and undoes its work itself if the curtain lifts.
	ready bool
	// frame holds the frame style bits the curtain took off.
	frame uintptr
	// outer is the window's rectangle before its frame came off, when the
	// curtain shrank it to its old client area; resized says it did.
	outer, client screenRect
	resized       bool
	tabRemoved    bool
}

// Windows draws a window's title bar and border even when its region is
// empty, so the curtain also takes the frame off while it is up.
const curtainFrameStyles = 0x00C00000 | 0x00040000 | 0x00080000 | 0x00020000 | 0x00010000 // WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX

const (
	pwClientOnly        = 0x1
	pwRenderFullContent = 0x2
	rdwInvalidate       = 0x0001
	rdwAllChildren      = 0x0080
	rdwFrame            = 0x0400
	desktopReachedName  = "desktop-reached"
)

// A first boot that asks for the user's own account shows Omarchy's account
// form in this window, so the curtain is only for disks that have reached
// the desktop before, and for the quick-start account, which asks nothing.
func startBootCurtain(cfg *config) {
	id, err := diskIdentity(cfg.disk)
	if err != nil {
		logf("startup: showing the window while Omarchy boots (disk identity: %v)", err)
		return
	}
	curtainMu.Lock()
	curtainDiskID, curtainVMDir = id, cfg.vmDir
	curtainMu.Unlock()
	if !cfg.instant && !desktopReachedBefore(cfg.vmDir, id) {
		logf("startup: showing the window while Omarchy boots (first desktop on this disk)")
		return
	}
	ui := getUI()
	ui.cancelMessage.Store(uiText("startup.cancel.confirm"))
	ui.cancelStatus.Store(uiText("startup.cancel.status"))
	curtainUp.Store(true)
	logf("startup: the window appears once the desktop session has started")
}

// diskIdentity names the disk file itself, not its path: a disk replaced
// under the same name (start over, restore) must count as new.
func diskIdentity(path string) (string, error) {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return "", err
	}
	// No access rights: only the file's identity is read, even while QEMU
	// has it open.
	handle, err := syscall.CreateFile(name, 0, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE,
		nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		return "", err
	}
	defer syscall.CloseHandle(handle)
	var info syscall.ByHandleFileInformation
	if err := syscall.GetFileInformationByHandle(handle, &info); err != nil {
		return "", err
	}
	return fmt.Sprintf("%08x-%08x%08x", info.VolumeSerialNumber, info.FileIndexHigh, info.FileIndexLow), nil
}

func desktopReachedBefore(vmDir, id string) bool {
	data, err := os.ReadFile(filepath.Join(vmDir, desktopReachedName))
	return err == nil && strings.TrimSpace(string(data)) == id
}

var markDesktopReached sync.Once

// desktopSessionStarted runs when the guest's clipboard bridge connects,
// which it does as the desktop session starts.
func desktopSessionStarted() {
	guestSessionStarted()
	markDesktopReached.Do(func() {
		curtainMu.Lock()
		id, vmDir := curtainDiskID, curtainVMDir
		curtainMu.Unlock()
		if id == "" || desktopReachedBefore(vmDir, id) {
			return
		}
		if err := os.WriteFile(filepath.Join(vmDir, desktopReachedName), []byte(id+"\n"), 0o644); err != nil {
			logf("startup: could not record that this disk reached the desktop: %v", err)
		}
	})
	wakeTitleEnforcer()
}

func wakeTitleEnforcer() {
	select {
	case qemuWindowChanged <- struct{}{}:
	default:
	}
}

// concealForCurtain hides a QEMU window: an empty region makes it invisible
// and click-through, and its frame comes off, because Windows still draws
// the title bar and border of a window with an empty region. It is not made
// layered (see the top of this file). It runs from the window hooks as QEMU
// creates and shows the window, before its first frame, and again from the
// title enforcer. The window calls run outside curtainMu, since they wait on
// QEMU's window thread.
func concealForCurtain(hwnd uintptr) {
	if !curtainUp.Load() {
		return
	}
	curtainMu.Lock()
	// The curtain may have lifted while this waited for the lock.
	if _, known := curtainWindows[hwnd]; known || !curtainUp.Load() {
		curtainMu.Unlock()
		return
	}
	window := &curtainWindow{}
	curtainWindows[hwnd] = window
	curtainMu.Unlock()

	hidden := hideCurtainWindow(hwnd, window)

	curtainMu.Lock()
	current := curtainWindows[hwnd] == window
	if !hidden && current {
		// Not recorded, so the next hook or title enforcer pass tries again.
		delete(curtainWindows, hwnd)
	}
	if hidden && current {
		window.ready = true
	}
	curtainMu.Unlock()
	// The curtain lifted, or the window was destroyed, while this hid it.
	if hidden && !current && isQemuDisplayWindow(hwnd, qemuPid.Load()) {
		showCurtainWindow(hwnd, window)
	}
	if !current {
		curtainMu.Lock()
		delete(curtainLifting, hwnd)
		curtainMu.Unlock()
		wakeTitleEnforcer()
	}
}

// curtainConcealed reports whether the curtain is hiding hwnd now, so the
// title enforcer leaves its placement alone while its frame is off.
func curtainConcealed(hwnd uintptr) bool {
	curtainMu.Lock()
	defer curtainMu.Unlock()
	_, ok := curtainWindows[hwnd]
	return ok && curtainUp.Load() || curtainLifting[hwnd]
}

func hideCurtainWindow(hwnd uintptr, window *curtainWindow) bool {
	region, _, err := procCreateRectRgn.Call(0, 0, 0, 0)
	if region == 0 {
		logf("startup: cannot hide the window while Omarchy boots: %v", err)
		return false
	}
	// The system owns the region once SetWindowRgn succeeds.
	if ok, _, err := procSetWindowRgn.Call(hwnd, region, 1); ok == 0 {
		procDeleteObject.Call(region)
		logf("startup: cannot hide the window while Omarchy boots: %v", err)
		return false
	}
	style, _, _ := procGetWindowLongW.Call(hwnd, gwlStyle)
	window.frame = style & curtainFrameStyles
	if window.frame == 0 {
		return true
	}
	// Taking the frame off would grow the client area, and with it the
	// guest's display. A window that is not maximized keeps its client
	// area: the window shrinks to it. A maximized one cannot.
	var outer screenRect
	client, clientOK := clientScreenRect(hwnd)
	zoomed, _, _ := procIsZoomed.Call(hwnd)
	gotOuter, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&outer)))
	procSetWindowLongW.Call(hwnd, gwlStyle, style&^curtainFrameStyles)
	if zoomed == 0 && gotOuter != 0 && clientOK {
		window.outer, window.client, window.resized = outer, client, true
		setWindowFrameRect(hwnd, client)
		return true
	}
	refreshWindowFrame(hwnd)
	return true
}

// showCurtainWindow undoes hideCurtainWindow: the full region, the frame bits
// the curtain took off (other style changes since are kept), and the window's
// old rectangle unless something moved or sized it since.
func showCurtainWindow(hwnd uintptr, window *curtainWindow) {
	// An empty region left behind would keep the window invisible for the
	// whole session, so retry before giving up.
	restored := false
	var err error
	for try := 0; try < 3 && !restored; try++ {
		if try > 0 {
			time.Sleep(50 * time.Millisecond)
		}
		var ok uintptr
		ok, _, err = procSetWindowRgn.Call(hwnd, 0, 1)
		restored = ok != 0
	}
	if !restored {
		logf("startup: cannot show the window again: %v", err)
	}
	if window.frame == 0 {
		return
	}
	style, _, _ := procGetWindowLongW.Call(hwnd, gwlStyle)
	procSetWindowLongW.Call(hwnd, gwlStyle, style|window.frame)
	if window.resized {
		var now screenRect
		if ok, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&now))); ok != 0 && now == window.client {
			setWindowFrameRect(hwnd, window.outer)
			return
		}
	}
	refreshWindowFrame(hwnd)
}

func clientScreenRect(hwnd uintptr) (screenRect, bool) {
	var client screenRect
	if ok, _, _ := procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&client))); ok == 0 {
		return client, false
	}
	origin := struct{ x, y int32 }{}
	if ok, _, _ := procClientToScreen.Call(hwnd, uintptr(unsafe.Pointer(&origin))); ok == 0 {
		return client, false
	}
	return screenRect{Left: origin.x, Top: origin.y, Right: origin.x + client.width(), Bottom: origin.y + client.height()}, true
}

const swpNoOwnerZOrder = 0x200

// setWindowFrameRect moves and sizes a window and makes Windows recompute its
// frame, without activating or reordering it.
func setWindowFrameRect(hwnd uintptr, r screenRect) {
	procSetWindowPos.Call(hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.width()), uintptr(r.height()), swpNoZorder|swpNoActivate|swpFrameChanged|swpNoOwnerZOrder)
}

// refreshWindowFrame makes Windows redraw a window's frame after its region
// or style changed, without moving, sizing, activating or reordering it.
func refreshWindowFrame(hwnd uintptr) {
	procSetWindowPos.Call(hwnd, 0, 0, 0, 0, 0, swpNoSize|swpNoMove|swpNoZorder|swpNoActivate|swpFrameChanged|swpNoOwnerZOrder)
}

// forgetCurtainWindow drops a destroyed window, so a new window that Windows
// gives the same handle is concealed again.
func forgetCurtainWindow(hwnd uintptr) {
	curtainMu.Lock()
	delete(curtainWindows, hwnd)
	delete(curtainLifting, hwnd)
	curtainMu.Unlock()
}

// curtainTaskbar takes the concealed window's button off the taskbar. It
// runs after the title enforcer gives the window its taskbar identity, which
// would otherwise bring the button back.
func curtainTaskbar(hwnd uintptr) {
	curtainMu.Lock()
	window := curtainWindows[hwnd]
	remove := window != nil && !window.tabRemoved
	if remove {
		window.tabRemoved = true
	}
	curtainMu.Unlock()
	if remove {
		taskbarTab(hwnd, false)
	}
}

// curtainTick runs on the title enforcer after each look at the windows.
// primary is the window of the guest's first display, or 0.
func curtainTick(primary uintptr) {
	if !curtainUp.Load() {
		watchDesktop(primary)
		return
	}
	times := curtainTimes()
	if !times.boot.Equal(curtainBoot) {
		curtainBoot = times.boot
	}
	now := time.Now()
	if reason := times.liftReason(now, false); reason != "" {
		liftCurtain(reason)
		return
	}
	// Cancel puts its own status up.
	if !setupCancelled() {
		status := uiText("status.starting_omarchy")
		switch {
		case runtimeLoaderPreparing.Load():
			status = uiText("status.preparing_runtime")
		case !times.boot.IsZero() && now.Sub(times.boot) >= curtainSlowNotice:
			status = uiText("status.slow_start")
		case times.checkPaint():
			status = uiText("status.starting_desktop")
		}
		if status != curtainStatus {
			curtainStatus = status
			uiStatus("%s", status)
		}
	}
	// The concealed window has an empty region, so there is no picture to
	// look at until the curtain lifts; watchDesktop takes over from there.
}

// watchDesktop keeps looking at a shown window every few seconds until it
// has kept showing the desktop for desktopSteadyWait in this boot. A
// minimized or hidden window is not looked at.
func watchDesktop(primary uintptr) {
	times := curtainTimes()
	if desktopSeen.Load() {
		return
	}
	// A hidden window is left out of the window list, so its time off
	// screen must not count toward the streak either.
	if primary == 0 {
		desktopShown.reset()
		return
	}
	if !times.checkPaint() || time.Since(desktopCheckedAt) < desktopCheckInterval {
		return
	}
	visible, _, _ := procIsWindowVisible.Call(primary)
	if iconic, _, _ := procIsIconic.Call(primary); iconic != 0 || visible == 0 {
		desktopShown.reset()
		return
	}
	// While the display is off or the session is locked every capture is
	// black. Those looks neither count nor break into the streak's timing.
	if !hostCapture.usable() {
		desktopShown.reset()
		return
	}
	if !curtainCapturing.CompareAndSwap(false, true) {
		return
	}
	desktopCheckedAt = time.Now()
	go func() {
		defer curtainCapturing.Store(false)
		painted := windowShowsDesktop(primary)
		if !hostCapture.usable() {
			desktopShown.reset()
			return
		}
		desktopLooks.Add(1)
		if desktopShown.look(time.Now(), painted) {
			desktopSeen.Store(true)
		}
	}()
}

// curtainPolling makes the title enforcer look more often while the
// desktop may appear at any moment.
func curtainPolling() bool {
	return curtainUp.Load() && curtainTimes().checkPaint()
}

// liftCurtain gives the guest's windows back their full region and taskbar
// buttons and closes the setup window.
func liftCurtain(reason string) {
	curtainMu.Lock()
	if !curtainUp.Load() {
		curtainMu.Unlock()
		return
	}
	curtainUp.Store(false)
	windows := map[uintptr]*curtainWindow{}
	for hwnd, window := range curtainWindows {
		// A window still being hidden is shown again by the goroutine
		// hiding it, once it sees the curtain has lifted.
		if window.ready {
			windows[hwnd] = window
		}
		curtainLifting[hwnd] = true
	}
	curtainWindows = map[uintptr]*curtainWindow{}
	curtainMu.Unlock()
	logf("startup: showing Omarchy (%s)", reason)
	// A window from an attempt that failed is gone, and its handle may
	// belong to another program by now.
	pid := qemuPid.Load()
	for hwnd := range windows {
		if !isQemuDisplayWindow(hwnd, pid) {
			delete(windows, hwnd)
			curtainMu.Lock()
			delete(curtainLifting, hwnd)
			curtainMu.Unlock()
		}
	}
	for hwnd, window := range windows {
		showCurtainWindow(hwnd, window)
		procRedrawWindow.Call(hwnd, 0, 0, rdwInvalidate|rdwAllChildren|rdwFrame)
		if window.tabRemoved {
			taskbarTab(hwnd, true)
		}
		// Only now may the title enforcer put the window back where it
		// belongs: its frame is part of the remembered placement.
		curtainMu.Lock()
		delete(curtainLifting, hwnd)
		curtainMu.Unlock()
	}
	wakeTitleEnforcer()
	if hwnd := qemuHwnd.Load(); hwnd != 0 {
		procSetForegroundWindow.Call(hwnd)
	}
	uiDone()
}

// windowShowsDesktop reads the window's picture through the compositor, as
// it would be on screen.
func windowShowsDesktop(hwnd uintptr) bool {
	var client struct{ left, top, right, bottom int32 }
	if ok, _, _ := procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&client))); ok == 0 {
		return false
	}
	width, height := int(client.right-client.left), int(client.bottom-client.top)
	if width < paintColumns || height < paintRows {
		return false
	}
	screen, _, _ := procGetDC.Call(0)
	if screen == 0 {
		return false
	}
	defer procReleaseDC.Call(0, screen)
	memory, _, _ := procCreateCompatibleDC.Call(screen)
	if memory == 0 {
		return false
	}
	defer procDeleteDC.Call(memory)
	header := struct {
		size                        uint32
		width, height               int32
		planes, bitCount            uint16
		compression, sizeImage      uint32
		xPerMeter, yPerMeter        int32
		colorsUsed, colorsImportant uint32
	}{width: int32(width), height: -int32(height), planes: 1, bitCount: 32}
	header.size = uint32(unsafe.Sizeof(header))
	var bits unsafe.Pointer
	bitmap, _, _ := procCreateDIBSection.Call(memory, uintptr(unsafe.Pointer(&header)), 0, uintptr(unsafe.Pointer(&bits)), 0, 0)
	if bitmap == 0 || bits == nil {
		return false
	}
	defer procDeleteObject.Call(bitmap)
	previous, _, _ := procSelectObject.Call(memory, bitmap)
	defer procSelectObject.Call(memory, previous)
	if ok, _, _ := procPrintWindow.Call(hwnd, memory, pwClientOnly|pwRenderFullContent); ok == 0 {
		return false
	}
	pixels := unsafe.Slice((*byte)(bits), width*height*4)
	return framePainted(pixels, width, height, width*4)
}

var (
	clsidTaskbarList = comGUID{0x56FDF344, 0xFD6D, 0x11d0, [8]byte{0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}}
	iidITaskbarList  = comGUID{0x56FDF342, 0xFD6D, 0x11d0, [8]byte{0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}}
)

// taskbarTab adds or removes a window's taskbar button.
func taskbarTab(hwnd uintptr, present bool) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	hr, _, _ := procCoInitializeEx.Call(0, 2)
	if int32(hr) >= 0 {
		defer procCoUninitialize.Call()
	} else if uint32(hr) != 0x80010106 { // An existing MTA is also usable.
		return
	}
	var list uintptr
	hr, _, _ = ole32.NewProc("CoCreateInstance").Call(uintptr(unsafe.Pointer(&clsidTaskbarList)), 0, 1,
		uintptr(unsafe.Pointer(&iidITaskbarList)), uintptr(unsafe.Pointer(&list)))
	if hr != 0 || list == 0 {
		logf("taskbar button: CoCreateInstance hr=%#x", hr)
		return
	}
	// ITaskbarList: 2 Release, 3 HrInit, 4 AddTab, 5 DeleteTab.
	defer comCall(list, 2)
	if hr := comCall(list, 3); hr != 0 {
		logf("taskbar button: HrInit hr=%#x", hr)
		return
	}
	method := 5
	if present {
		method = 4
	}
	comCall(list, method, hwnd)
}
