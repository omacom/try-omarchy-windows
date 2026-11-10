//go:build windows

package main

import (
	"context"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unicode/utf16"
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
	trayIconID                = 1
	trayCallbackMessage       = 0x8001 // WM_APP + 1
	trayNoticeMessage         = 0x8003
	traySettingsMessage       = 0x8004
	trayStopMessage           = 0x8002 // WM_APP + 2
	trayCommandShow           = 3001
	trayCommandShare          = 3002
	trayCommandSettings       = 3003
	trayCommandDiagnose       = 3004
	trayCommandShutdown       = 3005
	trayCommandReclaim        = 3006
	trayCommandReclaimStatus  = 3007
	trayCommandCameraStatus   = 3021
	trayCommandAbout          = 3020
	trayCommandHelp           = 3008
	trayCommandClipboardFiles = 3009
	trayCommandDevices        = 3010
	trayCommandTransfers      = 3011
	trayCommandResume         = 3012
	trayPowerStateMessage     = 0x8005
	trayCommandRestartUpdate  = 3022
	trayCommandInstallUpdate  = 3023

	nimAdd           = 0
	nimDelete        = 2
	nimSetVersion    = 4
	nifMessage       = 0x1
	nifIcon          = 0x2
	nifTip           = 0x4
	nifShowTip       = 0x80
	notifyVersion    = 4
	mfString         = 0
	mfGray           = 0x1
	mfSeparator      = 0x800
	tpmRightButton   = 0x2
	tpmReturnCmd     = 0x100
	wmContextMenu    = 0x007B
	wmPowerbroadcast = 0x0218
	wmNull           = 0x0000
	wmLButtonDblClk  = 0x0203
	wmRButtonUp      = 0x0205
	swRestore        = 9
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
	winqEmu  string
}

func trayControlArguments(cfg trayLaunchConfig, control string) []string {
	args := []string{}
	if cfg.portable {
		args = append(args, "-portable")
	} else {
		args = append(args, "-dir", cfg.dataDir)
		if control == "-settings" && cfg.winqEmu != "" {
			args = append(args, "-winq", cfg.winqEmu)
		}
	}
	return append(args, control)
}

var trayWindow atomic.Uintptr

type trayNotice struct{ title, message string }

var trayNotices struct {
	sync.Mutex
	pending []trayNotice
}

func showTrayNotice(title, message string) bool {
	hwnd := trayWindow.Load()
	if hwnd == 0 {
		return false
	}
	trayNotices.Lock()
	defer trayNotices.Unlock()
	trayNotices.pending = append(trayNotices.pending, trayNotice{title, message})
	if posted, _, _ := procPostMessageW.Call(hwnd, trayNoticeMessage, 0, 0); posted != 0 {
		return true
	}
	trayNotices.pending = trayNotices.pending[:len(trayNotices.pending)-1]
	return false
}

var transferErrorDialog atomic.Bool

func requestTraySettings() bool {
	hwnd := trayWindow.Load()
	if hwnd == 0 {
		return false
	}
	posted, _, err := procPostMessageW.Call(hwnd, traySettingsMessage, 0, 0)
	if posted == 0 {
		logf("tray: could not open settings from guest: %v", err)
	}
	return posted != 0
}

// Errors must never wait for a modal dialog in the file-transfer worker.
func reportTransferError(err error) {
	logf("file transfer: %v", err)
	message := uiTextWith("tray.transfer.failed", map[string]string{"error": err.Error()})
	if showTrayNotice(uiText("tray.transfer.title"), message) {
		return
	}
	if transferErrorDialog.CompareAndSwap(false, true) {
		go func() { defer transferErrorDialog.Store(false); infoBox(message) }()
	}
}

func notificationText(dst []uint16, text string) {
	data := utf16.Encode([]rune(strings.ReplaceAll(text, "\x00", "")))
	if len(data) >= len(dst) {
		data = data[:len(dst)-1]
		if n := len(data); n > 0 && data[n-1] >= 0xd800 && data[n-1] <= 0xdbff {
			data = data[:n-1]
		}
	}
	copy(dst, data)
}

// startTray keeps settings and diagnostics reachable after the setup window
// gives way to QEMU. Each action uses ordinary Windows UI and changes made in
// Settings take effect on the next VM launch.
func startTray(cfg *config) func() {
	ready := make(chan uintptr, 1)
	done := make(chan struct{})
	trayCfg := trayLaunchConfig{dataDir: cfg.dir, portable: cfg.portable, winqEmu: cfg.winqEmu}
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
	power := newGuestPowerState()
	powerCtx, cancelPower := context.WithCancel(context.Background())
	defer cancelPower()
	power.ctx = powerCtx
	powerEvents := make(chan uintptr, 8)
	const manualPowerResume = 0xffff

	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchyTray")
	taskbarName, _ := syscall.UTF16PtrFromString("TaskbarCreated")
	taskbarCreated, _, _ := procRegisterWindowMessage.Call(uintptr(unsafe.Pointer(taskbarName)))

	var hwnd uintptr
	unregisterPower := func() {}
	unregisterCapture := func() {}
	var nid notifyIconData
	var aboutOpen atomic.Bool
	var settingsOpen, diagnosticsOpen, devicesOpen atomic.Bool
	var noticeDetails string
	const retryTimer = 1
	retry := trayAddRetry{}

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

	retryAdd := func() {
		user32.NewProc("KillTimer").Call(hwnd, retryTimer)
		if retry.afterAdd(addIcon()) {
			if timer, _, err := user32.NewProc("SetTimer").Call(hwnd, retryTimer, 2000, 0); timer == 0 {
				logf("tray: retry timer failed: %v", err)
			}
		}
	}

	launchControl := func(flag string, running *atomic.Bool) {
		if !running.CompareAndSwap(false, true) {
			return
		}
		self, err := os.Executable()
		if err != nil {
			running.Store(false)
			errorBox(uiTextWith("tray.error.open_window", map[string]string{"error": err.Error()}))
			return
		}
		args := trayControlArguments(cfg, flag)
		cmd := exec.Command(self, args...)
		// Suppress a debug-build console without passing SW_HIDE to the
		// requested Settings, About, recovery, or device window.
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			running.Store(false)
			errorBox(uiTextWith("tray.error.open_window", map[string]string{"error": err.Error()}))
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

	// A guest reboot can change the shared folder (boot_settings_windows.go).
	openSharedFolder := func() {
		share := currentShare()
		if share == "" {
			return
		}
		cmd := exec.Command("explorer.exe", share)
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			errorBox(uiTextWith("tray.error.open_share", map[string]string{"error": err.Error()}))
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
		appendItem(mfString, trayCommandShow, uiText("tray.menu.open"))
		shareFlags := uintptr(mfString)
		if currentShare() == "" {
			shareFlags |= mfGray
		}
		appendItem(shareFlags, trayCommandShare, uiText("tray.menu.share"))
		appendItem(mfSeparator, 0, "")
		appendItem(mfString, trayCommandSettings, uiText("tray.menu.settings"))
		appendItem(mfString, trayCommandCameraStatus, uiText("tray.menu.camera"))
		appendItem(mfString, trayCommandDevices, uiText("tray.menu.devices"))
		appendItem(mfString, trayCommandTransfers, uiText("tray.menu.transfers"))
		appendItem(mfString, trayCommandDiagnose, uiText("tray.menu.diagnostics"))
		reclaimFlags := uintptr(mfString)
		if !reclaimSupported.Load() {
			reclaimFlags |= mfGray
		}
		appendItem(reclaimFlags, trayCommandReclaim, uiText("tray.menu.reclaim"))
		appendItem(reclaimFlags, trayCommandReclaimStatus, uiText("tray.menu.reclaim_status"))
		appendItem(mfString, trayCommandClipboardFiles, uiText("tray.menu.received_files"))
		appendItem(mfString, trayCommandAbout, uiText("tray.menu.about"))
		if !cfg.portable {
			appendItem(mfString, trayCommandInstallUpdate, uiText("tray.menu.install_update"))
		}
		if updateAvailable.Load() {
			appendItem(mfString, trayCommandRestartUpdate, uiText("tray.menu.restart_update"))
		}
		appendItem(mfString, trayCommandHelp, uiText("tray.menu.help"))
		appendItem(mfSeparator, 0, "")
		if HostPowerState().Phase == "recovery" {
			appendItem(mfString, trayCommandResume, uiText("tray.menu.resume"))
		}
		appendItem(mfString, trayCommandShutdown, uiText("tray.menu.shutdown"))

		var point struct{ x, y int32 }
		procGetCursorPos.Call(uintptr(unsafe.Pointer(&point)))
		procSetForegroundWindow.Call(hwnd)
		command, _, _ := procTrackPopupMenu.Call(menu, tpmRightButton|tpmReturnCmd,
			uintptr(point.x), uintptr(point.y), 0, hwnd, 0)
		procPostMessageW.Call(hwnd, wmNull, 0, 0)
		switch command {
		case trayCommandShow:
			if qemuWindow := qemuHwnd.Load(); qemuWindow != 0 {
				liftCurtain("asked for from the tray")
				// Approved Windows apps minimize a fullscreen VM. SW_SHOW leaves
				// a minimized window minimized; restore it before focusing it.
				procShowWindow.Call(qemuWindow, swRestore)
				procSetForegroundWindow.Call(qemuWindow)
			}
		case trayCommandShare:
			openSharedFolder()
		case trayCommandTransfers:
			showTrayTransfers()
		case trayCommandDevices:
			launchControl("-devices", &devicesOpen)
		case trayCommandSettings:
			launchControl("-settings", &settingsOpen)
		case trayCommandDiagnose:
			launchControl("-diagnostics", &diagnosticsOpen)
		case trayCommandReclaim:
			if msgBox(uiText("tray.reclaim.confirm"), mbYesNo|mbIconQuestion|mbDefbutton2) == idYes {
				if err := requestReclaimError(); err != nil {
					infoBox(err.Error())
				} else {
					infoBox(uiText("tray.reclaim.started"))
				}
			}
		case trayCommandReclaimStatus:
			if a := theAgent.Load(); a != nil {
				infoBox(a.reclaimStatus())
			} else {
				infoBox(uiText("tray.reclaim.not_ready"))
			}
		case trayCommandCameraStatus:
			infoBox(cameraStatusText())
		case trayCommandAbout:
			launchControl("-about", &aboutOpen)
		case trayCommandHelp:
			infoBox(uiText("help.everyday"))
		case trayCommandClipboardFiles:
			if dir, err := clipboardFilesCache(); err == nil {
				if err = os.MkdirAll(dir, 0700); err == nil {
					cmd := exec.Command("explorer.exe", dir)
					if cmd.Start() == nil {
						_ = cmd.Process.Release()
					}
				}
			}
		case trayCommandResume:
			select {
			case powerEvents <- manualPowerResume:
			default:
			}
		case trayCommandInstallUpdate:
			installUpdateFromFolder(cfg, hwnd)
		case trayCommandRestartUpdate:
			if msgBox(uiText("update.restart.confirm"), mbYesNo|mbIconQuestion|mbDefbutton2) == idYes {
				restartForUpdate.Store(true)
				intentionalUpdateQuit.Store(true)
				cancelBackgroundUpdate()
				requestSetupCancel()
			}
		case trayCommandShutdown:
			if qemuHwnd.Load() == 0 {
				infoBox(uiText("tray.shutdown.starting"))
			} else {
				requestQuitConfirm()
			}
		}
	}

	wndProc := syscall.NewCallback(func(window, message, wParam, lParam uintptr) uintptr {
		if captureWindowMessage(uint32(message), wParam, lParam) {
			if message == wmPowerbroadcast {
				return 1
			}
			return 0
		}
		if taskbarCreated != 0 && message == uintptr(taskbarCreated) {
			retry = trayAddRetry{}
			retryAdd()
			return 0
		}
		switch message {
		case 0x113: // WM_TIMER
			if wParam == retryTimer {
				retryAdd()
			}
			return 0
		case traySettingsMessage:
			launchControl("-settings", &settingsOpen)
			return 0
		case trayNoticeMessage:
			trayNotices.Lock()
			var text *trayNotice
			if len(trayNotices.pending) > 0 {
				text = &trayNotices.pending[0]
				trayNotices.pending = trayNotices.pending[1:]
			}
			trayNotices.Unlock()
			if text != nil {
				noticeDetails = text.message
				notice := nid
				notice.flags = 0x10  // NIF_INFO
				notice.infoFlags = 2 // NIIF_WARNING
				notificationText(notice.info[:], text.message)
				notificationText(notice.infoTitle[:], text.title)
				if ok, _, err := procShellNotifyIconW.Call(1, uintptr(unsafe.Pointer(&notice))); ok == 0 {
					logf("tray: notification failed: %v", err)
				}
			}
			return 0
		case trayCallbackMessage:
			event := uint32(lParam & 0xffff)
			switch event {
			case 0x405: // NIN_BALLOONUSERCLICK: the full rule list may exceed the balloon.
				if noticeDetails != "" {
					go infoBox(noticeDetails)
				}
			case wmLButtonDblClk:
				if qemuWindow := qemuHwnd.Load(); qemuWindow != 0 {
					liftCurtain("asked for from the tray")
					procShowWindow.Call(qemuWindow, swRestore)
					procSetForegroundWindow.Call(qemuWindow)
				}
			case wmRButtonUp, wmContextMenu:
				showMenu()
			}
			return 0
		case trayPowerStateMessage:
			nid.tip = [128]uint16{}
			tip := appTitle
			if HostPowerState().Phase == "recovery" {
				tip = uiText("tray.power.paused_title")
			} else if runtimeLoaderPreparing.Load() {
				tip = uiText("status.preparing_runtime")
			}
			notificationText(nid.tip[:], tip)
			notice := nid
			notice.flags = nifTip
			procShellNotifyIconW.Call(1, uintptr(unsafe.Pointer(&notice)))
			return 0
		case 0x0011: // WM_QUERYENDSESSION
			queryEndSession(window)
			return 1
		case 0x0016: // WM_ENDSESSION
			if wParam != 0 {
				cancelPower()
			}
			finishEndSession(window, wParam != 0)
			return 0
		case wmPowerbroadcast:
			select {
			case powerEvents <- wParam:
			default:
				logf("power: notification queue full")
			}
			return 1
		case trayStopMessage:
			procDestroyWindow.Call(window)
			return 0
		case wmDestroy:
			user32.NewProc("KillTimer").Call(window, retryTimer)
			unregisterPower()
			unregisterCapture()
			trayWindow.CompareAndSwap(window, 0)
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
	unregisterPower, err = registerPowerNotifications(hwnd)
	if err != nil {
		logf("power: %v; ordinary sleep broadcasts remain available", err)
	}
	defer unregisterPower()
	unregisterCapture = registerCaptureNotifications(hwnd)
	defer unregisterCapture()
	nid.size = uint32(unsafe.Sizeof(nid))
	nid.id = trayIconID
	nid.flags = nifMessage | nifIcon | nifTip | nifShowTip
	nid.callbackMessage = trayCallbackMessage
	var ownedIcon bool
	nid.icon, ownedIcon = selectTrayIcon(func() (uintptr, bool) {
		metric := comctl32.NewProc("LoadIconMetric")
		if metric.Find() != nil {
			return 0, false
		}
		var icon uintptr
		hr, _, _ := metric.Call(hInst, 1, 0, uintptr(unsafe.Pointer(&icon))) // LIM_SMALL
		return icon, int32(hr) >= 0
	}, func() uintptr { icon, _, _ := procLoadIconW.Call(hInst, 1); return icon })
	if ownedIcon {
		defer user32.NewProc("DestroyIcon").Call(nid.icon)
	}

	copy(nid.tip[:], syscall.StringToUTF16(appTitle))
	retryAdd()
	trayWindow.Store(hwnd)
	powerChanged := power.changed
	power.changed = func(phase string) {
		powerChanged(phase)
		procPostMessageW.Call(hwnd, trayPowerStateMessage, 0, 0)
	}
	power.onRecovery = func(err error) {
		if powerCtx.Err() != nil {
			return
		}
		showTrayNotice(uiText("tray.power.paused_title"), uiText("tray.power.paused"))
		go func() {
			bundle, snapshotErr := writeDiagnostics(cfg.dataDir, map[string]string{"event": "host-resume-failed", "power-error": err.Error(), "power-state": HostPowerState().Phase})
			if snapshotErr != nil {
				logf("power: automatic diagnostics failed: %v", snapshotErr)
				showTrayNotice(uiText("tray.power.paused_title"), uiTextWith("tray.power.snapshot_failed", map[string]string{"error": snapshotErr.Error()}))
			} else {
				logf("power: automatic diagnostics: %s", bundle)
				showTrayNotice(uiText("tray.power.paused_title"), uiTextWith("tray.power.snapshot", map[string]string{"path": bundle}))
			}
		}()
	}
	go func() {
		defer power.close()
		for {
			select {
			case <-powerCtx.Done():
				return
			case event := <-powerEvents:
				if event == manualPowerResume {
					power.manualResume()
				} else {
					power.handle(event)
				}
			}
		}
	}()

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

// The initial add plus ten timer retries leave TaskbarCreated available afterward.
type trayAddRetry struct{ retries int }

func (r *trayAddRetry) afterAdd(added bool) bool {
	if added || r.retries >= 10 {
		return false
	}
	r.retries++
	return true
}

func selectTrayIcon(metric func() (uintptr, bool), fallback func() uintptr) (uintptr, bool) {
	if icon, ok := metric(); ok && icon != 0 {
		return icon, true
	}
	return fallback(), false // LoadIconW returns a shared icon.
}
