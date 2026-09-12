//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

// The settings window (-settings): the rows of settings.json as plain Win32
// controls, the same rows the mac start menu has. It edits the file and
// nothing else; the next launch reads it. Plain system controls on purpose,
// so it behaves like every other Windows dialog with keyboard, high DPI and
// screen readers, unlike the custom-painted splash.

var (
	procIsDialogMessageW   = user32.NewProc("IsDialogMessageW")
	procEnableWindow       = user32.NewProc("EnableWindow")
	procLoadCursorW        = user32.NewProc("LoadCursorW")
	procGetStockObject     = syscall.NewLazyDLL("gdi32.dll").NewProc("GetStockObject")
	procSetFocus           = user32.NewProc("SetFocus")
	procAdjustWindowRectEx = user32.NewProc("AdjustWindowRectEx")
)

const (
	wsCaption             = 0x00C00000
	wsSysmenu             = 0x00080000
	wsBorder              = 0x00800000
	wsTabstop             = 0x00010000
	wsVscroll             = 0x00200000
	esAutohscroll         = 0x0080
	esMultiline           = 0x0004
	esAutovscroll         = 0x0040
	bsAutocheckbox        = 0x0003
	bsDefpushbutton       = 0x0001
	bmGetcheck            = 0x00F0
	bmSetcheck            = 0x00F1
	bstChecked            = 1
	idcArrow              = 32512
	colorBtnface          = 15
	defaultGuiFont        = 17
	wmGettextlength       = 0x000E
	wmGettext             = 0x000D
	settingsSaveID        = 2001
	settingsCancelID      = 2002
	settingsBrowseID      = 2003
	settingsFullID        = 2010
	settingsWindowID      = 2017
	settingsBorderlessID  = 2018
	settingsMemID         = 2011
	settingsShareID       = 2012
	settingsFwdID         = 2013
	settingsKeyID         = 2014
	settingsShareOnID     = 2015
	settingsDiskID        = 2016
	settingsBackupID      = 2020
	settingsRestoreID     = 2021
	settingsResetID       = 2022
	settingsRenderAutoID  = 2023
	settingsRenderGPUID   = 2024
	settingsRenderCPUID   = 2025
	settingsCPUsID        = 2026
	settingsUninstallID   = 2027
	settingsMoveID        = 2028
	settingsMoveCleanupID = 2029
	settingsHelpID        = 2030
	settingsSnapshotsID   = 2031
	settingsPortableID    = 2032
	settingsDisplaysID    = 2033
	settingsLANPublicID   = 2034
	settingsLANAddID      = 2035
	bsAutoradiobutton     = 0x0009
	wsGroup               = 0x00020000
	settingsRecoveryDone  = 0x8010
)

// runSettingsDialog shows the window and returns once it closes. saved is
// true when the file was written.
func runSettingsDialog(path, dataDir string, portable bool) (saved bool) {
	if !portable {
		if self, err := os.Executable(); err == nil {
			if resolved, err := prepareMovedLocation(filepath.Dir(self), false); err == nil && !pathsEqual(resolved, filepath.Dir(self)) {
				cmd := exec.Command(filepath.Join(resolved, stableLauncherName), "-dir", dataDir, "-settings")
				if err := cmd.Start(); err != nil {
					errorBox("Could not reopen moved Settings: " + err.Error())
				}
				return false
			}
		}
	}
	runtime.LockOSThread()
	guard, err := lockMoveStore(hostMoveStore())
	if err != nil {
		errorBox(err.Error())
		return false
	}
	defer guard.Close()
	if !portable {
		if err := checkMovedSettings(dataDir); err != nil {
			errorBox(err.Error())
			return false
		}
	}
	current, err := loadSettingsWithRepair(path)
	if err != nil {
		if errors.Is(err, errSetupCancelled) {
			return false
		}
		errorBox("Try Omarchy cannot read its settings:\n\n" + err.Error() + "\n\nFix or delete the file, then open the settings again.")
		return false
	}

	storage, err := loadStorageWithRepair(dataDir)
	if err != nil {
		if errors.Is(err, errSetupCancelled) {
			return false
		}
		errorBox("Try Omarchy cannot read its storage preferences:\n\n" + err.Error())
		return false
	}
	guard.Close()
	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var hwnd uintptr
	var scroll settingsScroll
	var hWindow, hFull, hBorderless, hMem, hCPUs, hDisk, hShare, hShareOn, hFwd, hKey uintptr
	var hRenderAuto, hRenderGPU, hRenderCPU, hDisplays, hLANPublic uintptr

	text := func(handle uintptr) string {
		n, _, _ := procSendMessageW.Call(handle, wmGettextlength, 0, 0)
		buf := make([]uint16, n+1)
		procSendMessageW.Call(handle, wmGettext, uintptr(len(buf)), uintptr(unsafe.Pointer(&buf[0])))
		return syscall.UTF16ToString(buf)
	}
	setText := func(handle uintptr, value string) {
		t, _ := syscall.UTF16PtrFromString(value)
		procSendMessageW.Call(handle, wmSettext, 0, uintptr(unsafe.Pointer(t)))
	}
	collect := func() (settings, error) {
		fullscreen, _, _ := procSendMessageW.Call(hFull, bmGetcheck, 0, 0)
		borderless, _, _ := procSendMessageW.Call(hBorderless, bmGetcheck, 0, 0)
		shareChecked, _, _ := procSendMessageW.Call(hShareOn, bmGetcheck, 0, 0)
		render := renderAuto
		if r, _, _ := procSendMessageW.Call(hRenderGPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderGPU
		} else if r, _, _ := procSendMessageW.Call(hRenderCPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderCPU
		}
		s, err := settingsFromForm(fullscreen == bstChecked, borderless == bstChecked, shareChecked == bstChecked,
			text(hMem), text(hCPUs), text(hShare), text(hFwd), text(hKey), render)
		if err != nil {
			return s, err
		}
		s.Displays, err = strconv.Atoi(strings.TrimSpace(text(hDisplays)))
		if err != nil || s.Displays < 1 || s.Displays > maximumGuestDisplays {
			return s, fmt.Errorf("choose 1 to %d guest displays", maximumGuestDisplays)
		}
		checkedLAN, _, _ := procSendMessageW.Call(hLANPublic, bmGetcheck, 0, 0)
		s.LANPublic = checkedLAN == bstChecked
		s.ForwardAdapters = map[string]string{}
		for _, forward := range s.Forwards {
			if adapter := current.ForwardAdapters[forward]; adapter != "" {
				s.ForwardAdapters[forward] = adapter
			}
		}
		return s, s.validate()
	}
	browseFolder := func() {
		if selected, ok := browseForFolder(hwnd, "Choose the Windows folder to share with Omarchy"); ok {
			setText(hShare, selected)
			procSendMessageW.Call(hShareOn, bmSetcheck, bstChecked, 0)
		}
	}

	launchRecovery := func(action string) {
		self, err := os.Executable()
		if err != nil {
			errorBox(err.Error())
			return
		}
		args := []string{"-dir", dataDir, "-recovery", action}
		if portable {
			args = append(args, "-portable")
		}
		cmd := exec.Command(self, args...)
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
		if err = cmd.Start(); err != nil {
			errorBox("Could not open recovery controls:\n\n" + err.Error())
			return
		}
		procAllowSetForeground.Call(uintptr(cmd.Process.Pid))
		procEnableWindow.Call(hwnd, 0)
		go func() { _ = cmd.Wait(); procPostMessageW.Call(hwnd, settingsRecoveryDone, 0, 0) }()
	}

	wndProc := syscall.NewCallback(func(h, msg, wParam, lParam uintptr) uintptr {
		if scroll.handle(msg, wParam) {
			return 0
		}
		switch msg {
		case wmCommand:
			switch wParam & 0xffff {
			case settingsHelpID:
				infoBox(everydayHelp)
			case settingsSaveID:
				guard, err := lockMoveStore(hostMoveStore())
				if err != nil {
					errorBox(err.Error())
					return 0
				}
				defer guard.Close()
				if !portable {
					if err := checkMovedSettings(dataDir); err != nil {
						errorBox(err.Error())
						return 0
					}
				}
				s, err := collect()
				diskGiB := storage.DiskGiB
				if err == nil {
					diskGiB, err = parseDiskGiB(text(hDisk))
				}
				if err == nil && s.activeShare() != "" {
					home, homeErr := os.UserHomeDir()
					if homeErr != nil {
						err = fmt.Errorf("finding the Windows home folder: %w", homeErr)
					} else {
						s.Share, err = validateWindowsSharedFolder(s.Share, dataDir, home)
					}
				}
				if err == nil {
					err = saveSettings(path, s)
				}
				if err == nil && diskGiB != storage.DiskGiB {
					if storageErr := saveStorageSettings(dataDir, diskGiB); storageErr != nil {
						errorBox("Other settings were saved, but disk capacity could not be saved:\n\n" + storageErr.Error())
						return 0
					}
				}
				if err != nil {
					errorBox("These settings cannot be saved:\n\n" + err.Error())
					return 0
				}
				saved = true
				procDestroyWindow.Call(h)
			case settingsCancelID, idCancel:
				procDestroyWindow.Call(h)
			case settingsBrowseID:
				browseFolder()
			case settingsMoveID:
				launchRecovery("move")
			case settingsMoveCleanupID:
				launchRecovery("move-cleanup")
			case settingsLANAddID:
				value, err := chooseLANForward(hwnd)
				if err != nil {
					errorBox(err.Error())
					break
				}
				if value.Forward != "" {
					lines := append(strings.Fields(text(hFwd)), value.Forward)
					var forwards forwardList
					for _, line := range lines {
						if err = forwards.Set(line); err != nil {
							break
						}
					}
					if err != nil {
						errorBox(err.Error())
					} else {
						setText(hFwd, strings.Join(lines, "\r\n"))
						if value.Adapter != "" {
							if current.ForwardAdapters == nil {
								current.ForwardAdapters = map[string]string{}
							}
							current.ForwardAdapters[value.Forward] = value.Adapter
						}
					}
				}
			case settingsPortableID:
				launchRecovery("portable-create")
			case settingsSnapshotsID:
				launchRecovery("snapshots")
			case settingsBackupID:
				launchRecovery("backup")
			case settingsRestoreID:
				launchRecovery("restore")
			case settingsResetID:
				launchRecovery("reset")
			case settingsUninstallID:
				launchRecovery("uninstall")
			}
			return 0
		case settingsRecoveryDone:
			if !portable {
				if resolved, err := prepareMovedLocation(dataDir, false); err == nil && !pathsEqual(resolved, dataDir) {
					cmd := exec.Command(filepath.Join(resolved, stableLauncherName), "-dir", resolved, "-settings")
					if err := cmd.Start(); err != nil {
						errorBox("Open Settings at " + resolved + ": " + err.Error())
					}
					procDestroyWindow.Call(h)
					return 0
				}
			}
			procEnableWindow.Call(h, 1)
			procSetForegroundWindow.Call(h)
			return 0
		case wmClose:
			procDestroyWindow.Call(h)
			return 0
		case wmDestroy:
			procPostQuitMessage.Call(0)
			return 0
		}
		r, _, _ := procDefWindowProcW.Call(h, msg, wParam, lParam)
		return r
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
	cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
	icon, _, _ := procLoadIconW.Call(hInst, 1)
	wc := wndclassex{
		size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst,
		icon: icon, cursor: cursor, brush: colorBtnface + 1, className: className, iconSm: icon,
	}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("settings: RegisterClassExW failed: %v", err)
		return false
	}

	const clientW, clientH = 480, 780
	rect := [4]int32{0, 0, clientW, clientH}
	style := uintptr(wsCaption | wsSysmenu | wsVscroll)
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	// AdjustWindowRectEx excludes the vertical scrollbar from its calculation.
	scrollbarWidth, _, _ := procGetSystemMetrics.Call(2) // SM_CXVSCROLL
	w, hgt := rect[2]-rect[0]+int32(scrollbarWidth), rect[3]-rect[1]
	sx, _, _ := procGetSystemMetrics.Call(smCxscreen)
	sy, _, _ := procGetSystemMetrics.Call(smCyscreen)
	work := [4]int32{0, 0, int32(sx), int32(sy)}
	procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work[0])), 0)
	if available := work[3] - work[1] - 16; hgt > available {
		hgt = available
	}
	scroll.height = hgt - (rect[3] - rect[1] - clientH)
	scroll.content = clientH
	x := work[0] + (work[2]-work[0]-w)/2
	yWindow := work[1] + (work[3]-work[1]-hgt)/2
	title, _ := syscall.UTF16PtrFromString(appTitle + " settings")
	var err2 error
	hwnd, _, err2 = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		style|wsVisible, uintptr(x), uintptr(yWindow), uintptr(w), uintptr(hgt), 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("settings: CreateWindowExW failed: %v", err2)
		return false
	}

	scroll.window = hwnd
	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	mk := func(class, label string, x, y, cx, cy int32, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		t, _ := syscall.UTF16PtrFromString(label)
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(t)),
			wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(cx), uintptr(cy), hwnd, id, hInst, 0)
		procSendMessageW.Call(h, wmSetfont, font, 1)
		scroll.controls = append(scroll.controls, settingsScrollControl{h, x, y, cx, cy})
		return h
	}
	const left, labelW, fieldX, fieldW = 16, 150, 170, 294
	y := int32(16)
	mk("STATIC", "Window mode", left, y+3, labelW, 20, ssNoprefix, 0)
	hWindow = mk("BUTTON", "Windowed", fieldX, y, 90, 22, bsAutoradiobutton|wsGroup|wsTabstop, settingsWindowID)
	hFull = mk("BUTTON", "Fullscreen", fieldX+96, y, 90, 22, bsAutoradiobutton, settingsFullID)
	hBorderless = mk("BUTTON", "Borderless", fieldX+192, y, 90, 22, bsAutoradiobutton, settingsBorderlessID)
	if current.Fullscreen {
		procSendMessageW.Call(hFull, bmSetcheck, bstChecked, 0)
	} else if current.Borderless {
		procSendMessageW.Call(hBorderless, bmSetcheck, bstChecked, 0)
	} else {
		procSendMessageW.Call(hWindow, bmSetcheck, bstChecked, 0)
	}
	y += 30
	mk("STATIC", "Guest displays", left, y+3, labelW, 20, ssNoprefix, 0)
	hDisplays = mk("EDIT", strconv.Itoa(guestDisplayCount(current.Displays)), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsDisplaysID)
	mk("STATIC", "1 to 16 displays", fieldX+112, y+3, fieldW-112, 20, ssNoprefix, 0)
	y += 34
	mk("STATIC", "Rendering", left, y+3, labelW, 20, ssNoprefix, 0)
	hRenderAuto = mk("BUTTON", "Automatic", fieldX, y, 90, 22, bsAutoradiobutton|wsGroup|wsTabstop, settingsRenderAutoID)
	hRenderGPU = mk("BUTTON", "GPU", fieldX+96, y, 60, 22, bsAutoradiobutton, settingsRenderGPUID)
	hRenderCPU = mk("BUTTON", "CPU", fieldX+162, y, 60, 22, bsAutoradiobutton, settingsRenderCPUID)
	switch current.Render {
	case renderGPU:
		procSendMessageW.Call(hRenderGPU, bmSetcheck, bstChecked, 0)
	case renderCPU:
		procSendMessageW.Call(hRenderCPU, bmSetcheck, bstChecked, 0)
	default:
		procSendMessageW.Call(hRenderAuto, bmSetcheck, bstChecked, 0)
	}
	y += 24
	mk("STATIC", "Automatic tries the GPU and remembers when this PC cannot use it. GPU retries every launch.", left, y, clientW-2*left, 36, ssNoprefix, 0)
	y += 44
	mk("STATIC", "Guest memory (MiB)", left, y+3, labelW, 20, ssNoprefix, 0)
	hMem = mk("EDIT", strconv.Itoa(current.MemoryMiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsMemID)
	mk("STATIC", "0 = automatic", fieldX+112, y+3, fieldW-112, 20, ssNoprefix, 0)
	y += 34
	mk("STATIC", "Guest CPUs", left, y+3, labelW, 20, ssNoprefix, 0)
	hCPUs = mk("EDIT", strconv.Itoa(current.CPUs), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsCPUsID)
	mk("STATIC", fmt.Sprintf("0 = automatic (%d of %d)", pickGuestCPUs(runtime.NumCPU()), runtime.NumCPU()), fieldX+112, y+3, fieldW-112, 20, ssNoprefix, 0)
	y += 34
	mk("STATIC", "Disk capacity (GiB)", left, y+3, labelW, 20, ssNoprefix, 0)
	hDisk = mk("EDIT", strconv.Itoa(storage.DiskGiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsDiskID)
	y += 28
	capacityHelp := "0 keeps the default. Increasing grows the disk next launch; lowering never shrinks it. Space is used as files are added."
	mk("STATIC", capacityHelp, left, y, clientW-2*left, 36, ssNoprefix, 0)
	y += 38
	status := ""
	if disk, err := inspectInstallationDisk(dataDir); err == nil {
		status = "Current capacity: " + formatGiB(disk.VirtualBytes) + ". "
	}
	if available, err := diskFreeBytes(dataDir); err == nil {
		status += "Free on Windows drive: " + formatGiB(available) + "."
	}
	mk("STATIC", status, left, y, clientW-2*left, 20, ssNoprefix, 0)
	y += 26
	mk("STATIC", "Location", left, y+3, labelW, 20, ssNoprefix, 0)
	mk("EDIT", dataDir, fieldX, y, fieldW, 22, wsBorder|wsTabstop|esAutohscroll|0x0800, 0) // ES_READONLY; long paths remain selectable.
	y += 24
	mk("STATIC", "Shared folder", left, y+3, labelW, 20, ssNoprefix, 0)
	hShare = mk("EDIT", current.Share, fieldX, y, fieldW-80, 24, wsBorder|wsTabstop|esAutohscroll, settingsShareID)
	mk("BUTTON", "Browse...", fieldX+fieldW-72, y, 72, 24, wsTabstop, settingsBrowseID)
	y += 28
	hShareOn = mk("BUTTON", "Allow Omarchy to read and change this folder", fieldX, y, fieldW, 22,
		bsAutocheckbox|wsTabstop, settingsShareOnID)
	if current.Share != "" && !current.ShareDisabled {
		procSendMessageW.Call(hShareOn, bmSetcheck, bstChecked, 0)
	}
	y += 34
	mk("STATIC", "Port forwards\nLocal: tcp:2222:22\nLAN: tcp:IP:8080:80", left, y+3, labelW, 60, ssNoprefix, 0)
	hFwd = mk("EDIT", strings.Join(current.Forwards, "\r\n"), fieldX, y, fieldW, 72,
		wsBorder|wsTabstop|wsVscroll|esMultiline|esAutovscroll, settingsFwdID)
	y += 82
	mk("BUTTON", "Add LAN...", left, y, 120, 26, wsTabstop, settingsLANAddID)
	hLANPublic = mk("BUTTON", "Allow LAN on public networks", left+130, y, clientW-2*left-130, 22, bsAutocheckbox|wsTabstop, settingsLANPublicID)
	if current.LANPublic {
		procSendMessageW.Call(hLANPublic, bmSetcheck, bstChecked, 0)
	}
	y += 32
	mk("STATIC", "SSH public key file\n(blank: your ~/.ssh/id_*.pub)", left, y+3, labelW, 40, ssNoprefix, 0)
	hKey = mk("EDIT", current.SSHKey, fieldX, y, fieldW, 24, wsBorder|wsTabstop|esAutohscroll, settingsKeyID)
	// The two-line key label above is 40 px tall from y+3; start the next
	// row below it or the label's second line paints over this text.
	y += 50
	mk("STATIC", "Changes apply the next time Omarchy starts.",
		left, y, clientW-2*left, 20, ssNoprefix, 0)
	y += 30
	mk("STATIC", "Backup and recovery", left, y, clientW-2*left, 20, ssNoprefix, 0)
	y += 24
	for _, control := range []struct {
		label string
		id    uintptr
		x     int32
	}{{"Back up...", settingsBackupID, left}, {"Restore...", settingsRestoreID, left + 112}, {"Snapshots...", settingsSnapshotsID, left + 224}, {"Reset guest...", settingsResetID, left + 336}} {
		button := mk("BUTTON", control.label, control.x, y, 104, 26, wsTabstop, control.id)
		if portable && control.id == settingsResetID {
			procEnableWindow.Call(button, 0)
		}
	}
	y += 30
	help := "Close Omarchy first. Backups use saved settings. Restore creates a separate copy."
	if portable {
		help = "Backups and snapshots create independent copies. Close Omarchy first."
	}
	mk("STATIC", help, left, y, clientW-2*left, 36, ssNoprefix, 0)
	y += 42
	uninstallButton := mk("BUTTON", "Uninstall...", left, y, 104, 26, wsTabstop, settingsUninstallID)
	moveButton := mk("BUTTON", "Move...", left+112, y, 104, 26, wsTabstop, settingsMoveID)
	cleanupButton := mk("BUTTON", "Clean up...", left+224, y, 104, 26, wsTabstop, settingsMoveCleanupID)
	mk("BUTTON", "Portable copy...", left+336, y, 104, 26, wsTabstop, settingsPortableID)
	state, stateErr := hostMoveStore().load()
	if portable {
		procEnableWindow.Call(uninstallButton, 0)
		procEnableWindow.Call(moveButton, 0)
	}
	if portable || stateErr != nil || state.Retained == nil || !state.Retained.Booted || !pathsEqual(state.Retained.Destination, dataDir) {
		procEnableWindow.Call(cleanupButton, 0)
	}
	mk("BUTTON", "Help and shortcuts", left, clientH-40, 150, 26, wsTabstop, settingsHelpID)
	mk("BUTTON", "Save", clientW-16-180, clientH-40, 84, 26, bsDefpushbutton|wsTabstop, settingsSaveID)
	mk("BUTTON", "Cancel", clientW-16-84, clientH-40, 84, 26, wsTabstop, settingsCancelID)
	scroll.move(0)
	// Settings is often opened from the tray while the maximized QEMU window
	// owns the foreground. Raise it once, then immediately return it to the
	// normal z-order so it is visible without staying above unrelated apps.
	procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetForegroundWindow.Call(hwnd)
	procSetWindowPos.Call(hwnd, hwndNotTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetFocus.Call(hWindow)

	var m msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if ok, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&m))); ok != 0 {
			// IsDialogMessage also dispatches scrolling and paint messages.
			// Only keyboard navigation should bring the focused control back.
			if m.message == wmKeydown || m.message == wmSyskeydown || m.message == 0x106 {
				scroll.revealFocus()
			}
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
	return saved
}
