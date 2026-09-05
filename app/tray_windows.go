//go:build windows

package main

import (
	"os"
	"os/exec"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

var (
	procShellNotifyIconW      = shell32.NewProc("Shell_NotifyIconW")
	procCreatePopupMenu       = user32.NewProc("CreatePopupMenu")
	procAppendMenuW           = user32.NewProc("AppendMenuW")
	procTrackPopupMenu        = user32.NewProc("TrackPopupMenu")
	procDestroyMenu           = user32.NewProc("DestroyMenu")
	procGetCursorPos          = user32.NewProc("GetCursorPos")
	procPostMessageW          = user32.NewProc("PostMessageW")
	procRegisterWindowMessage = user32.NewProc("RegisterWindowMessageW")
	procAllowSetForeground    = user32.NewProc("AllowSetForegroundWindow")
)

const (
	trayIconID          = 1
	trayCallbackMessage = 0x8001 // WM_APP + 1
	trayStopMessage     = 0x8002 // WM_APP + 2
	trayCommandShow     = 3001
	trayCommandShare    = 3002
	trayCommandSettings = 3003
	trayCommandDiagnose = 3004
	trayCommandShutdown = 3005
	trayCommandReclaim  = 3006

	nimAdd                = 0
	nimDelete             = 2
	nimSetVersion         = 4
	nifMessage            = 0x1
	nifIcon               = 0x2
	nifTip                = 0x4
	nifShowTip            = 0x80
	notifyVersion         = 4
	mfString              = 0
	mfGray                = 0x1
	mfSeparator           = 0x800
	tpmRightButton        = 0x2
	tpmReturnCmd          = 0x100
	wmContextMenu         = 0x007B
	wmPowerbroadcast      = 0x0218
	pbtApmResumeSuspend   = 0x0007
	pbtApmResumeAutomatic = 0x0012
	wmNull                = 0x0000
	wmLButtonDblClk       = 0x0203
	wmRButtonUp           = 0x0205
)

type trayGUID struct {
	d1 uint32
	d2 uint16
	d3 uint16
	d4 [8]byte
}

type notifyIconData struct {
	size            uint32
	hwnd            uintptr
	id              uint32
	flags           uint32
	callbackMessage uint32
	icon            uintptr
	tip             [128]uint16
	state           uint32
	stateMask       uint32
	info            [256]uint16
	version         uint32
	infoTitle       [64]uint16
	infoFlags       uint32
	guid            trayGUID
	balloonIcon     uintptr
}

type trayLaunchConfig struct {
	dataDir  string
	portable bool
	share    string
}

// startTray keeps settings and diagnostics reachable after the setup window
// gives way to QEMU. Each action uses ordinary Windows UI and changes made in
// Settings take effect on the next VM launch.
func startTray(cfg *config) func() {
	ready := make(chan uintptr, 1)
	done := make(chan struct{})
	trayCfg := trayLaunchConfig{dataDir: cfg.dir, portable: cfg.portable, share: cfg.share}
	go runTray(trayCfg, ready, done)
	hwnd := <-ready
	if hwnd == 0 {
		return func() {}
	}
	var once sync.Once
	return func() {
		once.Do(func() {
			if posted, _, err := procPostMessageW.Call(hwnd, trayStopMessage, 0, 0); posted == 0 {
				logf("tray: stop message failed: %v", err)
				return
			}
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				logf("tray: timed out waiting for the message loop to stop")
			}
		})
	}
}

func runTray(cfg trayLaunchConfig, ready chan<- uintptr, done chan<- struct{}) {
	runtime.LockOSThread()
	defer close(done)

	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchyTray")
	taskbarName, _ := syscall.UTF16PtrFromString("TaskbarCreated")
	taskbarCreated, _, _ := procRegisterWindowMessage.Call(uintptr(unsafe.Pointer(taskbarName)))

	var hwnd uintptr
	var nid notifyIconData
	var settingsOpen, diagnosticsOpen atomic.Bool

	addIcon := func() bool {
		if hwnd == 0 {
			return false
		}
		nid.hwnd = hwnd
		if result, _, err := procShellNotifyIconW.Call(nimAdd, uintptr(unsafe.Pointer(&nid))); result == 0 {
			logf("tray: Shell_NotifyIconW add failed: %v", err)
			return false
		}
		nid.version = notifyVersion
		procShellNotifyIconW.Call(nimSetVersion, uintptr(unsafe.Pointer(&nid)))
		return true
	}

	launchControl := func(flag string, running *atomic.Bool) {
		if !running.CompareAndSwap(false, true) {
			return
		}
		self, err := os.Executable()
		if err != nil {
			running.Store(false)
			errorBox("Try Omarchy could not open " + flag + ".\n\n" + err.Error())
			return
		}
		args := []string{}
		if cfg.portable {
			args = append(args, "-portable")
		} else {
			args = append(args, "-dir", cfg.dataDir)
		}
		args = append(args, flag)
		cmd := exec.Command(self, args...)
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			running.Store(false)
			errorBox("Try Omarchy could not open " + flag + ".\n\n" + err.Error())
			return
		}
		// The tray action is user initiated, but the child has a different PID.
		// Transfer foreground permission so its window does not open behind the
		// maximized VM on Windows 11.
		if allowed, _, err := procAllowSetForeground.Call(uintptr(cmd.Process.Pid)); allowed == 0 {
			logf("tray: foreground handoff to %s failed: %v", flag, err)
		}
		go func() {
			_ = cmd.Wait()
			running.Store(false)
		}()
	}

	openSharedFolder := func() {
		if cfg.share == "" {
			return
		}
		cmd := exec.Command("explorer.exe", cfg.share)
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			errorBox("Windows could not open the shared folder.\n\n" + err.Error())
			return
		}
		_ = cmd.Process.Release()
	}

	showMenu := func() {
		menu, _, _ := procCreatePopupMenu.Call()
		if menu == 0 {
			return
		}
		defer procDestroyMenu.Call(menu)
		appendItem := func(flags, id uintptr, label string) {
			text, _ := syscall.UTF16PtrFromString(label)
			procAppendMenuW.Call(menu, flags, id, uintptr(unsafe.Pointer(text)))
		}
		appendItem(mfString, trayCommandShow, "Open Omarchy")
		shareFlags := uintptr(mfString)
		if cfg.share == "" {
			shareFlags |= mfGray
		}
		appendItem(shareFlags, trayCommandShare, "Open Shared Folder")
		appendItem(mfSeparator, 0, "")
		appendItem(mfString, trayCommandSettings, "Settings...")
		appendItem(mfString, trayCommandDiagnose, "Create diagnostics...")
		appendItem(mfString, trayCommandReclaim, "Reclaim disk space...")
		appendItem(mfSeparator, 0, "")
		appendItem(mfString, trayCommandShutdown, "Shut down Omarchy...")

		var point struct{ x, y int32 }
		procGetCursorPos.Call(uintptr(unsafe.Pointer(&point)))
		procSetForegroundWindow.Call(hwnd)
		command, _, _ := procTrackPopupMenu.Call(menu, tpmRightButton|tpmReturnCmd,
			uintptr(point.x), uintptr(point.y), 0, hwnd, 0)
		procPostMessageW.Call(hwnd, wmNull, 0, 0)
		switch command {
		case trayCommandShow:
			if qemuWindow := qemuHwnd.Load(); qemuWindow != 0 {
				procShowWindow.Call(qemuWindow, swShow)
				procSetForegroundWindow.Call(qemuWindow)
			}
		case trayCommandShare:
			openSharedFolder()
		case trayCommandSettings:
			launchControl("-settings", &settingsOpen)
		case trayCommandDiagnose:
			launchControl("-diagnostics", &diagnosticsOpen)
		case trayCommandReclaim:
			if msgBox("Give deleted Omarchy files' space back to Windows?\n\nOmarchy will write zeros over its free space now, which takes a few minutes and briefly fills its disk. The disk file on Windows shrinks the next time Omarchy shuts down.", mbYesNo|mbIconQuestion|mbDefbutton2) == idYes {
				if !requestReclaim() {
					infoBox("Omarchy is not ready for this yet. It needs the current guest update, a moment after startup, and at least 4 GiB free on the Windows drive; try again shortly.")
				}
			}
		case trayCommandShutdown:
			if qemuHwnd.Load() == 0 {
				infoBox("Omarchy is still starting. You can shut it down once its window opens.")
			} else {
				requestQuitConfirm()
			}
		}
	}

	wndProc := syscall.NewCallback(func(window, message, wParam, lParam uintptr) uintptr {
		if message == uintptr(taskbarCreated) {
			addIcon()
			return 0
		}
		switch message {
		case trayCallbackMessage:
			event := uint32(lParam & 0xffff)
			switch event {
			case wmLButtonDblClk:
				if qemuWindow := qemuHwnd.Load(); qemuWindow != 0 {
					procShowWindow.Call(qemuWindow, swShow)
					procSetForegroundWindow.Call(qemuWindow)
				}
			case wmRButtonUp, wmContextMenu:
				showMenu()
			}
			return 0
		case wmPowerbroadcast:
			if wParam == pbtApmResumeAutomatic || wParam == pbtApmResumeSuspend {
				logf("windows resumed from sleep")
				select {
				case hostResumed <- struct{}{}:
				default:
				}
			}
			return 1
		case trayStopMessage:
			procDestroyWindow.Call(window)
			return 0
		case wmDestroy:
			procShellNotifyIconW.Call(nimDelete, uintptr(unsafe.Pointer(&nid)))
			procPostQuitMessage.Call(0)
			return 0
		}
		result, _, _ := procDefWindowProcW.Call(window, message, wParam, lParam)
		return result
	})

	type wndclassex struct {
		size, style         uint32
		wndProc             uintptr
		clsExtra, wndExtra  int32
		inst                uintptr
		icon, cursor, brush uintptr
		menuName, className *uint16
		iconSm              uintptr
	}
	wc := wndclassex{size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst, className: className}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("tray: RegisterClassExW failed: %v", err)
		ready <- 0
		return
	}
	hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(className)),
		0, 0, 0, 0, 0, 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("tray: CreateWindowExW failed: %v", err)
		ready <- 0
		return
	}
	nid.size = uint32(unsafe.Sizeof(nid))
	nid.id = trayIconID
	nid.flags = nifMessage | nifIcon | nifTip | nifShowTip
	nid.callbackMessage = trayCallbackMessage
	nid.icon, _, _ = procLoadIconW.Call(hInst, 1)
	copy(nid.tip[:], syscall.StringToUTF16(appTitle))
	if !addIcon() {
		procDestroyWindow.Call(hwnd)
		ready <- 0
		return
	}
	logf("tray: ready")
	ready <- hwnd

	var message msgStruct
	for {
		result, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if result == 0 || int32(result) == -1 {
			break
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
	logf("tray: stopped")
}
