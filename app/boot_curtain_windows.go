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
// watches, and a hidden window has no picture to look at. Instead the window
// stays shown, in its real place and size, but fully transparent and
// click-through, with no taskbar button. Windows still composes it, so its
// picture can be read to tell when the wallpaper is drawn.

var (
	procSetLayeredWindowAttributes = user32.NewProc("SetLayeredWindowAttributes")
	procGetWindowLongW             = user32.NewProc("GetWindowLongW")
	procSetWindowLongW             = user32.NewProc("SetWindowLongW")
	procPrintWindow                = user32.NewProc("PrintWindow")
	procGetDC                      = user32.NewProc("GetDC")
	procReleaseDC                  = user32.NewProc("ReleaseDC")
	gdi32                          = syscall.NewLazyDLL("gdi32.dll")
	procCreateCompatibleDC         = gdi32.NewProc("CreateCompatibleDC")
	procCreateDIBSection           = gdi32.NewProc("CreateDIBSection")
	procDeleteDC                   = gdi32.NewProc("DeleteDC")

	// curtainUp is true from launch until the window is shown for the first
	// time. Reboots after that show the guest's window as they always did.
	curtainUp      atomic.Bool
	curtainMu      sync.Mutex
	curtainWindows = map[uintptr]*curtainWindow{}
	// The disk this launch boots and its folder, for recording that it
	// reached the desktop.
	curtainDiskID, curtainVMDir string
	// Consecutive frames that showed the desktop, and whether a capture is
	// still running (a busy QEMU can hold one up).
	curtainPainted   atomic.Int32
	curtainCapturing atomic.Bool
	curtainBoot      time.Time
	curtainStatus    string
	// When watchDesktop last looked at the window after the curtain lifted.
	desktopCheckedAt time.Time
)

type curtainWindow struct {
	exStyle    uintptr
	tabRemoved bool
}

const (
	gwlExStyle          = ^uintptr(19) // GWL_EXSTYLE (-20)
	wsExLayered         = 0x00080000
	wsExTransparent     = 0x00000020
	lwaAlpha            = 0x2
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
	logf("startup: the window appears once the desktop is drawn")
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

// concealForCurtain makes a QEMU window invisible and click-through. It runs
// from the window hooks as QEMU creates and shows the window, before its
// first frame, and again from the title enforcer.
func concealForCurtain(hwnd uintptr) {
	if !curtainUp.Load() {
		return
	}
	curtainMu.Lock()
	defer curtainMu.Unlock()
	// The curtain may have lifted while this waited for the lock.
	if _, done := curtainWindows[hwnd]; done || !curtainUp.Load() {
		return
	}
	style, _, _ := procGetWindowLongW.Call(hwnd, gwlExStyle)
	curtainWindows[hwnd] = &curtainWindow{exStyle: style}
	procSetWindowLongW.Call(hwnd, gwlExStyle, style|wsExLayered|wsExTransparent)
	procSetLayeredWindowAttributes.Call(hwnd, 0, 0, lwaAlpha)
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
		curtainPainted.Store(0)
	}
	now := time.Now()
	if reason := times.liftReason(now, curtainPainted.Load() >= 2); reason != "" {
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
	if primary != 0 && times.checkPaint() && curtainCapturing.CompareAndSwap(false, true) {
		go func() {
			defer curtainCapturing.Store(false)
			painted := windowShowsDesktop(primary)
			if painted {
				curtainPainted.Add(1)
			} else {
				curtainPainted.Store(0)
			}
			if desktopShown.look(time.Now(), painted) {
				desktopSeen.Store(true)
			}
			wakeTitleEnforcer()
		}()
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
	if !curtainCapturing.CompareAndSwap(false, true) {
		return
	}
	desktopCheckedAt = time.Now()
	go func() {
		defer curtainCapturing.Store(false)
		painted := windowShowsDesktop(primary)
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

// liftCurtain fades the guest's windows in, gives them back their taskbar
// buttons and closes the setup window.
func liftCurtain(reason string) {
	curtainMu.Lock()
	if !curtainUp.Load() {
		curtainMu.Unlock()
		return
	}
	curtainUp.Store(false)
	windows := curtainWindows
	curtainWindows = map[uintptr]*curtainWindow{}
	curtainMu.Unlock()
	logf("startup: showing Omarchy (%s)", reason)
	// A window from an attempt that failed is gone, and its handle may
	// belong to another program by now.
	pid := qemuPid.Load()
	for hwnd := range windows {
		if !isQemuDisplayWindow(hwnd, pid) {
			delete(windows, hwnd)
		}
	}
	for step := 1; step <= 8; step++ {
		for hwnd := range windows {
			procSetLayeredWindowAttributes.Call(hwnd, 0, uintptr(step*255/8), lwaAlpha)
		}
		time.Sleep(20 * time.Millisecond)
	}
	for hwnd, window := range windows {
		procSetWindowLongW.Call(hwnd, gwlExStyle, window.exStyle)
		procRedrawWindow.Call(hwnd, 0, 0, rdwInvalidate|rdwAllChildren|rdwFrame)
		if window.tabRemoved {
			taskbarTab(hwnd, true)
		}
	}
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
