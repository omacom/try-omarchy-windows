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
//
// Windows carries out the region, style and position calls on QEMU's window
// thread, which goes on setting up the guest for a second or more after it
// creates the window and handles no messages until then. A window that QEMU
// showed at once stayed on screen with its frame and no region for that time
// (#413). A runtime that can hold its windows (heldWindowsPatch) creates them
// hidden while the curtain is up and marks them with heldWindowProperty; the
// launcher shows such a window only once the curtain has hidden it. An older
// runtime shows its windows itself and the curtain hides them as before.
//
// Nothing QEMU presents reaches a window with an empty region, so when the
// region came back the window showed what it held from before, black or
// white, until QEMU's next frame. A runtime with heldWindowFramePatch also
// cloaks a held window and uncloaks it once it has presented a frame on
// screen. The setup window then stays up until QEMU's windows are no longer
// cloaked, at most curtainFrameWait, and the taskbar button, the full screen
// mark and the focus follow then: the shell leaves cloaked windows alone.

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
	procGetPropW           = user32.NewProc("GetPropW")
	procDwmGetWindowAttr   = syscall.NewLazyDLL("dwmapi.dll").NewProc("DwmGetWindowAttribute")
	heldWindowProperty, _  = syscall.UTF16PtrFromString("TryOmarchy.HeldWindow")

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
	// curtainWaitsForFrame is set when QEMU keeps its held windows cloaked
	// until their first frame (heldWindowFramePatch).
	curtainWaitsForFrame atomic.Bool
	// The lift waiting for QEMU to uncloak its windows, under curtainMu.
	curtainRevealing *curtainReveal
)

type curtainReveal struct {
	windows map[uintptr]*curtainWindow
	lifted  time.Time
}

type curtainWindow struct {
	// ready is set once the window is hidden. Until then the goroutine
	// hiding it owns it, and undoes its work itself if the curtain lifts.
	ready bool
	// frame holds the frame style bits the curtain took off, and style the
	// window's style when it did.
	frame, style uintptr
	// outer is the window's rectangle before its frame came off, when the
	// curtain shrank it to its old client area; resized says it did.
	outer, client screenRect
	resized       bool
	// tabRemoved says the curtain took the window's taskbar button off;
	// tabLast is when it last did (curtainTaskbar).
	tabRemoved bool
	tabLast    time.Time
	// border is the size of the frame on each side, and bordered says it
	// was measured. fitted is set once the window has been given the client
	// area it will have when shown (fitCurtainWindows).
	border           screenRect
	bordered, fitted bool
	// placed is set when the title enforcer gave the frameless window its
	// placement (placeConcealedWindow): outer and client are then where it
	// goes when shown, whatever QEMU resizes it to meanwhile.
	placed bool
	// offMonitor is set once the window was moved off the whole monitor.
	offMonitor bool
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
// QEMU's window thread. A held window is shown once it is hidden.
func concealForCurtain(hwnd uintptr) {
	if !curtainUp.Load() {
		return
	}
	curtainMu.Lock()
	// The curtain may have lifted while this waited for the lock.
	if window, known := curtainWindows[hwnd]; known || !curtainUp.Load() {
		// Whoever is still hiding it shows it when done.
		hidden := known && window.ready
		curtainMu.Unlock()
		if hidden {
			showHeldWindow(hwnd)
		}
		return
	}
	window := &curtainWindow{}
	curtainWindows[hwnd] = window
	curtainMu.Unlock()

	started := time.Now()
	hidden := hideCurtainWindow(hwnd, window)
	// The window shows until this returns, and it waits on QEMU's window
	// thread, which may be busy setting up the display.
	if took := time.Since(started); hidden && took >= 250*time.Millisecond {
		logf("startup: hiding the window took %v", took.Round(time.Millisecond))
	}

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
	// A held window appears now that it is hidden. One that could not be
	// hidden stays held: a later try shows it, or QEMU does after
	// heldWindowWait.
	if hidden && current {
		showHeldWindow(hwnd)
	}
	// The curtain lifted, or the window was destroyed, while this hid it.
	if hidden && !current && isQemuDisplayWindow(hwnd, qemuPid.Load()) {
		showCurtainWindow(hwnd, window)
		showHeldWindow(hwnd)
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

// placeConcealedWindow gives a window the curtain has already hidden, frame
// and all, the placement the title enforcer would give a shown one, and
// returns the placement to restore after the lift (nil if it did nothing).
// Windows' own placement calls go wrong on a frameless window: a maximized
// one covers the whole monitor, which can make Windows drive the display as
// for a full-screen game, and the remembered rectangle becomes its client
// area. So the window gets the client area it will have when shown, the
// curtain remembers its framed rectangle for the lift, and a maximized
// placement is applied after the lift (placementAfterCurtain). This is the
// usual order with a runtime that holds its windows: the curtain hides them
// before they are shown, and so before the enforcer ever sees them.
func placeConcealedWindow(hwnd uintptr, p *windowPlacement) *windowPlacement {
	curtainMu.Lock()
	window := curtainWindows[hwnd]
	ok := curtainUp.Load() && window != nil && window.ready && window.frame != 0 && window.bordered
	var frame screenRect
	if ok {
		frame = window.border
	}
	curtainMu.Unlock()
	if !ok {
		return nil
	}
	target := p
	if target == nil {
		// As a new window is shown: maximized, restoring to where QEMU put it.
		now := capturePlacement(hwnd)
		if now == nil {
			return nil
		}
		target = &windowPlacement{Schema: 1, Normal: now.Normal, Maximized: true}
	}
	// The remembered rectangle is in workspace coordinates; so is this one.
	if !applyPlacement(hwnd, &windowPlacement{Normal: frameless(target.Normal, frame)}) {
		return nil
	}
	var client screenRect
	if ok, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&client))); ok == 0 {
		return nil
	}
	outer := screenRect{client.Left - frame.Left, client.Top - frame.Top, client.Right + frame.Right, client.Bottom + frame.Bottom}
	if target.Maximized {
		monitor, ok := hostMonitorForWindow(hwnd)
		if !ok {
			return nil
		}
		outer, client = shownWindowRects(client, true, monitor.Work, frame)
		setWindowFrameRect(hwnd, client)
	}
	curtainMu.Lock()
	if curtainWindows[hwnd] == window {
		window.outer, window.client, window.resized, window.placed = outer, client, true, true
	}
	curtainMu.Unlock()
	logf("startup: placed the hidden window at its shown client area, %dx%d (maximized %v)", client.width(), client.height(), target.Maximized)
	return target
}

// keepOffMonitor moves hidden windows that cover their whole monitor, which
// QEMU can do when it sizes a frameless window to the guest's display, to
// the client area they will have when shown. Fullscreen windows cover their
// monitor by the user's choice and are left alone.
func keepOffMonitor() {
	if enumTitleFullscreen {
		parkFullscreenWindows()
		return
	}
	type check struct {
		hwnd               uintptr
		window             *curtainWindow
		frame, client      screenRect
		knowClient, logged bool
	}
	var checks []check
	curtainMu.Lock()
	for hwnd, window := range curtainWindows {
		if window.ready && window.bordered {
			// Only a placed window's client area is where it will be shown;
			// another's is the size QEMU first gave it.
			checks = append(checks, check{hwnd, window, window.border, window.client, window.placed, window.offMonitor})
		}
	}
	curtainMu.Unlock()
	for _, c := range checks {
		var now screenRect
		if ok, _, _ := procGetWindowRect.Call(c.hwnd, uintptr(unsafe.Pointer(&now))); ok == 0 {
			continue
		}
		monitor, ok := hostMonitorForWindow(c.hwnd)
		if !ok {
			continue
		}
		outer, target, move := hiddenWindowOffMonitor(now, monitor, c.frame, c.client, c.knowClient)
		if !move {
			continue
		}
		setWindowFrameRect(c.hwnd, target)
		curtainMu.Lock()
		c.window.offMonitor = true
		if !c.knowClient {
			// The lift puts the frame back around this area. Without this
			// it keeps the area from before a display change and leaves the
			// shown window a frame too small.
			c.window.outer, c.window.client, c.window.resized = outer, target, true
		}
		curtainMu.Unlock()
		if !c.logged {
			logf("startup: the hidden window covered its monitor; moved it to %dx%d", target.width(), target.height())
		}
	}
}

// parkFullscreenWindows gives a hidden full screen window the rectangle it
// keeps once shown (growFullscreenWindow), so the guest draws at that size
// behind the curtain and nothing moves at the lift.
func parkFullscreenWindows() {
	var hwnds []uintptr
	curtainMu.Lock()
	for hwnd, window := range curtainWindows {
		if window.ready {
			hwnds = append(hwnds, hwnd)
		}
	}
	curtainMu.Unlock()
	for _, hwnd := range hwnds {
		growFullscreenWindow(hwnd)
	}
}

// fullscreenGrowLogged is the last rectangle growFullscreenWindow logged, so
// the log shows each new one once.
var fullscreenGrowLogged atomic.Value

// growFullscreenWindow gives a frameless window that exactly covers its
// monitor the fullscreenGrown rectangle. It reports whether it moved it.
func growFullscreenWindow(hwnd uintptr) bool {
	var now screenRect
	if ok, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&now))); ok == 0 {
		return false
	}
	monitor, ok := hostMonitorForWindow(hwnd)
	if !ok || now != monitor.Bounds {
		return false
	}
	style, _, _ := procGetWindowLongW.Call(hwnd, gwlStyle)
	if maximized, _, _ := procIsZoomed.Call(hwnd); maximized != 0 || style&wsCaption == wsCaption {
		return false
	}
	var others []screenRect
	for _, m := range hostMonitors() {
		if m.Bounds != monitor.Bounds {
			others = append(others, m.Bounds)
		}
	}
	hidden := ""
	if bar, ok := autoHideTaskbarRect(monitor.Bounds); ok {
		hidden = taskbarEdge(bar, monitor.Bounds)
	}
	r, edge := fullscreenGrown(monitor, others, hidden)
	if ok, _, _ := procSetWindowPos.Call(hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.width()), uintptr(r.height()), swpNoZorder|swpNoActivate|swpNoOwnerZOrder); ok == 0 {
		return false
	}
	if line := fmt.Sprintf("%v %v", r, now); fullscreenGrowLogged.Swap(line) != line {
		logf("full screen: kept the window one line larger than its %dx%d monitor, at the %s edge (%d,%d %dx%d)", now.width(), now.height(), edge, r.Left, r.Top, r.width(), r.height())
	}
	return true
}

// windowHeld reports whether QEMU created hwnd hidden for the launcher to
// show. QEMU drops the mark once the window is shown.
func windowHeld(hwnd uintptr) bool {
	held, _, _ := procGetPropW.Call(hwnd, uintptr(unsafe.Pointer(heldWindowProperty)))
	return held != 0
}

// releaseHeldWindow shows a window QEMU holds, once the curtain has hidden
// it, or at once when the curtain is down. It runs from the window hooks and
// the title enforcer, which leaves hidden windows alone otherwise.
func releaseHeldWindow(hwnd uintptr) {
	if !windowHeld(hwnd) {
		return
	}
	concealForCurtain(hwnd)
	if !curtainUp.Load() {
		showHeldWindow(hwnd)
	}
}

func showHeldWindow(hwnd uintptr) {
	if visible, _, _ := procIsWindowVisible.Call(hwnd); visible != 0 || !windowHeld(hwnd) {
		return
	}
	// As SDL would have shown it. The call waits for QEMU's window thread.
	procShowWindow.Call(hwnd, swShow)
	if curtainUp.Load() {
		logf("startup: showed QEMU's window behind the curtain")
	}
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
	window.frame, window.style = style&curtainFrameStyles, style
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
	if gotOuter != 0 && clientOK {
		window.border = screenRect{client.Left - outer.Left, client.Top - outer.Top, outer.Right - client.Right, outer.Bottom - client.Bottom}
		window.bordered = true
	}
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
	frame := curtainFrameBack(window.frame, window.style, style)
	if frame == 0 {
		logf("startup: the window went full screen behind the curtain and keeps no frame")
		return
	}
	procSetWindowLongW.Call(hwnd, gwlStyle, style|frame)
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
	taskbarFullscreen.forget(hwnd)
	curtainMu.Lock()
	delete(curtainWindows, hwnd)
	delete(curtainLifting, hwnd)
	curtainMu.Unlock()
}

// curtainTabMu orders the curtain's taskbar button deletes with the lift
// that adds the button back.
var curtainTabMu sync.Mutex

// curtainTaskbar takes the concealed window's button off the taskbar. It
// runs on each title enforcer pass, after the first one gives the window its
// taskbar identity, which would otherwise bring the button back. The taskbar
// takes that identity up a moment later and can then show the button again,
// so the button is taken off again until the lift (curtainTabDeleteDue). The
// deletes leave Windows not taking a full screen window for the full screen
// app once it is shown; syncFullscreenMark tells the taskbar instead.
func curtainTaskbar(hwnd uintptr) {
	// The lift adds the button back under the same lock, so a delete cannot
	// land after it: once the curtain is down the window is no longer in
	// curtainWindows.
	curtainTabMu.Lock()
	defer curtainTabMu.Unlock()
	now := time.Now()
	curtainMu.Lock()
	window := curtainWindows[hwnd]
	remove := window != nil && curtainTabDeleteDue(window.tabLast, now)
	if remove {
		window.tabRemoved = true
		window.tabLast = now
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
		finishCurtainReveal()
		if !curtainRevealPending() {
			watchDesktop(primary)
		}
		return
	}
	times := curtainTimes()
	if !times.boot.Equal(curtainBoot) {
		curtainBoot = times.boot
	}
	now := time.Now()
	if reason := times.liftReason(now); reason != "" {
		liftCurtain(reason)
		return
	}
	// From now on the guest takes the size of the window.
	if guestFollowsWindow.Load() && !enumTitleFullscreen {
		fitCurtainWindows()
	}
	keepOffMonitor()
	// Cancel puts its own status up.
	if !setupCancelled() {
		status := uiText("status.starting_omarchy")
		switch {
		case runtimeLoaderPreparing.Load():
			status = uiText("status.preparing_runtime")
		case times.slowStart(now):
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
	// look at: the guest's agent says when the desktop is drawn, and
	// watchDesktop looks at the shown window from there.
}

// fitCurtainWindows gives each hidden window, once, the client area it will
// have when the curtain lifts, so the guest draws its desktop at that size
// and does not redraw it as the window appears. Without its frame a
// maximized window covers the whole monitor, and one the title enforcer
// placed takes its framed rectangle as its client area. It runs once the
// guest follows the window: before that QEMU resizes the window to the
// guest's boot display.
func fitCurtainWindows() {
	type fit struct {
		hwnd   uintptr
		window *curtainWindow
		border screenRect
	}
	var fits []fit
	curtainMu.Lock()
	for hwnd, window := range curtainWindows {
		if window.ready && window.bordered && !window.fitted {
			window.fitted = true
			fits = append(fits, fit{hwnd, window, window.border})
		}
	}
	curtainMu.Unlock()
	for _, f := range fits {
		var now screenRect
		if ok, _, _ := procGetWindowRect.Call(f.hwnd, uintptr(unsafe.Pointer(&now))); ok == 0 {
			continue
		}
		zoomed, _, _ := procIsZoomed.Call(f.hwnd)
		curtainMu.Lock()
		// The curtain shrank it to its old client area and nothing moved it
		// since: the frame goes back around that area.
		keep := f.window.resized && now == f.window.client
		placed, placedClient := f.window.placed, f.window.client
		curtainMu.Unlock()
		// Placed by placeConcealedWindow: QEMU may have sized it to the
		// guest's display since, and taking that for its framed rectangle
		// would leave the guest a frame too small.
		if placed {
			if now != placedClient {
				setWindowFrameRect(f.hwnd, placedClient)
				logf("startup: sized the hidden window to its shown client area, %dx%d", placedClient.width(), placedClient.height())
			}
			continue
		}
		if keep && zoomed == 0 {
			continue
		}
		var work screenRect
		if zoomed != 0 {
			monitor, ok := hostMonitorForWindow(f.hwnd)
			if !ok {
				continue
			}
			work = monitor.Work
		}
		outer, client := shownWindowRects(now, zoomed != 0, work, f.border)
		if client.width() <= 0 || client.height() <= 0 || now == client {
			continue
		}
		setWindowFrameRect(f.hwnd, client)
		curtainMu.Lock()
		if curtainWindows[f.hwnd] == f.window {
			f.window.outer, f.window.client, f.window.resized = outer, client, true
		}
		curtainMu.Unlock()
		logf("startup: sized the hidden window to its shown client area, %dx%d", client.width(), client.height())
	}
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
	return curtainUp.Load() && curtainTimes().checkPaint() || curtainRevealPending()
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
	waits := curtainWaitsForFrame.Load() && len(windows) > 0
	if waits {
		curtainMu.Lock()
		curtainRevealing = &curtainReveal{windows: windows, lifted: time.Now()}
		curtainMu.Unlock()
	}
	for hwnd, window := range windows {
		showCurtainWindow(hwnd, window)
		procRedrawWindow.Call(hwnd, 0, 0, rdwInvalidate|rdwAllChildren|rdwFrame)
		if !waits {
			showCurtainTab(hwnd, window)
		}
		// Only now may the title enforcer put the window back where it
		// belongs: its frame is part of the remembered placement.
		curtainMu.Lock()
		delete(curtainLifting, hwnd)
		curtainMu.Unlock()
	}
	wakeTitleEnforcer()
	if !waits {
		handOverFromCurtain()
	}
}

// showCurtainTab gives a window the curtain hid its taskbar button and full
// screen mark back.
func showCurtainTab(hwnd uintptr, window *curtainWindow) {
	if window.tabRemoved {
		curtainTabMu.Lock()
		taskbarTab(hwnd, true)
		curtainTabMu.Unlock()
	}
	syncFullscreenMark(hwnd)
}

func handOverFromCurtain() {
	if hwnd := qemuHwnd.Load(); hwnd != 0 {
		procSetForegroundWindow.Call(hwnd)
	}
	uiDone()
}

// curtainRevealPending reports whether the lift is waiting for QEMU to show
// its windows; the setup window and the full screen mark wait with it.
func curtainRevealPending() bool {
	curtainMu.Lock()
	defer curtainMu.Unlock()
	return curtainRevealing != nil
}

// windowCloakedByApp reports whether QEMU cloaked hwnd. A window the shell
// cloaks, on another virtual desktop, or whose state cannot be read, does
// not count.
func windowCloakedByApp(hwnd uintptr) bool {
	const dwmwaCloaked, dwmCloakedApp = 14, 1
	var cloaked uint32
	hr, _, _ := procDwmGetWindowAttr.Call(hwnd, dwmwaCloaked, uintptr(unsafe.Pointer(&cloaked)), 4)
	return hr == 0 && cloaked&dwmCloakedApp != 0
}

// finishCurtainReveal ends the lift once QEMU shows its windows: their
// taskbar buttons come back, Omarchy gets the focus and the setup window
// closes. It runs when a window is uncloaked and on each title enforcer
// pass, which also catches the time limit.
func finishCurtainReveal() {
	curtainMu.Lock()
	reveal := curtainRevealing
	curtainMu.Unlock()
	if reveal == nil {
		return
	}
	pid := qemuPid.Load()
	var cloaked []bool
	for hwnd := range reveal.windows {
		if isQemuDisplayWindow(hwnd, pid) {
			cloaked = append(cloaked, windowCloakedByApp(hwnd))
		}
	}
	done, late := curtainRevealDone(cloaked, reveal.lifted, time.Now())
	if !done {
		return
	}
	curtainMu.Lock()
	if curtainRevealing != reveal {
		curtainMu.Unlock()
		return
	}
	curtainRevealing = nil
	curtainMu.Unlock()
	took := time.Since(reveal.lifted).Round(time.Millisecond)
	if late {
		logf("startup: QEMU had not shown Omarchy's window %v after the lift; showing Omarchy anyway", took)
	} else {
		logf("startup: QEMU showed Omarchy's window with its first frame %v after the lift", took)
	}
	for hwnd, window := range reveal.windows {
		if isQemuDisplayWindow(hwnd, pid) {
			showCurtainTab(hwnd, window)
		}
	}
	handOverFromCurtain()
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
	iidITaskbarList2 = comGUID{0x602D4995, 0xB13A, 0x429b, [8]byte{0xA6, 0x6E, 0x19, 0x35, 0xE4, 0x4F, 0x43, 0x17}}
)

// taskbarTab adds or removes a window's taskbar button.
func taskbarTab(hwnd uintptr, present bool) {
	method := 5
	if present {
		method = 4
	}
	taskbarListCall(method, hwnd)
}

var taskbarFullscreen fullscreenMarks

// syncFullscreenMark tells the taskbar whether a shown QEMU window is full
// screen, when that changed. Windows decides by itself on a window it saw
// enter full screen, but not on one whose button the curtain took off, and
// then leaves the taskbar over it. The taskbar keeps the mark per window and
// lets other windows over it as usual while the window is not active.
func syncFullscreenMark(hwnd uintptr) {
	fullscreen := displayWindowFullscreen(hwnd)
	if taskbarFullscreen.differs(hwnd, fullscreen) && taskbarMarkFullscreen(hwnd, fullscreen) {
		taskbarFullscreen.set(hwnd, fullscreen)
		logf("full screen: told the taskbar the window is full screen: %v", fullscreen)
	}
}

// clearFullscreenMarks takes back every mark, so the taskbar never stays
// behind a window that outlives the launcher.
func clearFullscreenMarks() {
	for _, hwnd := range taskbarFullscreen.clear() {
		taskbarMarkFullscreen(hwnd, false)
	}
}

// curtainHidesWindows reports whether the curtain is hiding a window, so the
// title enforcer keeps the pace of curtainTaskbar's deletes.
func curtainHidesWindows() bool {
	curtainMu.Lock()
	defer curtainMu.Unlock()
	return curtainUp.Load() && len(curtainWindows) > 0
}

// taskbarMarkFullscreen tells the taskbar that hwnd is, or no longer is, a
// full screen window, so it goes behind the window while it is active.
func taskbarMarkFullscreen(hwnd uintptr, on bool) bool {
	flag := uintptr(0)
	if on {
		flag = 1
	}
	return taskbarListCall(8, hwnd, flag)
}

// taskbarListCall calls one ITaskbarList2 method and reports whether it
// succeeded.
func taskbarListCall(method int, args ...uintptr) bool {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	hr, _, _ := procCoInitializeEx.Call(0, 2)
	if int32(hr) >= 0 {
		defer procCoUninitialize.Call()
	} else if uint32(hr) != 0x80010106 { // An existing MTA is also usable.
		return false
	}
	var list uintptr
	hr, _, _ = ole32.NewProc("CoCreateInstance").Call(uintptr(unsafe.Pointer(&clsidTaskbarList)), 0, 1,
		uintptr(unsafe.Pointer(&iidITaskbarList2)), uintptr(unsafe.Pointer(&list)))
	if hr != 0 || list == 0 {
		logf("taskbar button: CoCreateInstance hr=%#x", hr)
		return false
	}
	// ITaskbarList2: 2 Release, 3 HrInit, 4 AddTab, 5 DeleteTab,
	// 8 MarkFullscreenWindow.
	defer comCall(list, 2)
	if hr := comCall(list, 3); hr != 0 {
		logf("taskbar button: HrInit hr=%#x", hr)
		return false
	}
	return comCall(list, method, args...) == 0
}
