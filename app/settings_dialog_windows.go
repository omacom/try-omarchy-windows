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
	wsCaption            = 0x00C00000
	wsSysmenu            = 0x00080000
	wsBorder             = 0x00800000
	wsTabstop            = 0x00010000
	wsVscroll            = 0x00200000
	esAutohscroll        = 0x0080
	esMultiline          = 0x0004
	esAutovscroll        = 0x0040
	bsAutocheckbox       = 0x0003
	bsDefpushbutton      = 0x0001
	bmGetcheck           = 0x00F0
	bmSetcheck           = 0x00F1
	bstChecked           = 1
	idcArrow             = 32512
	colorBtnface         = 15
	defaultGuiFont       = 17
	wmGettextlength      = 0x000E
	wmGettext            = 0x000D
	settingsSaveID       = 2001
	settingsCancelID     = 2002
	settingsBrowseID     = 2003
	settingsFullID       = 2010
	settingsMemID        = 2011
	settingsShareID      = 2012
	settingsFwdID        = 2013
	settingsKeyID        = 2014
	settingsShareOnID    = 2015
	settingsDiskID       = 2016
	settingsBackupID     = 2020
	settingsRestoreID    = 2021
	settingsResetID      = 2022
	settingsRenderAutoID = 2023
	settingsRenderGPUID  = 2024
	settingsRenderCPUID  = 2025
	bsAutoradiobutton    = 0x0009
	wsGroup              = 0x00020000
	settingsRecoveryDone = 0x8010
)

// runSettingsDialog shows the window and returns once it closes. saved is
// true when the file was written.
func runSettingsDialog(path, dataDir string, portable bool) (saved bool) {
	runtime.LockOSThread()
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
	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var hwnd uintptr
	var hFull, hMem, hDisk, hShare, hShareOn, hFwd, hKey uintptr
	var hRenderAuto, hRenderGPU, hRenderCPU uintptr

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
		checked, _, _ := procSendMessageW.Call(hFull, bmGetcheck, 0, 0)
		shareChecked, _, _ := procSendMessageW.Call(hShareOn, bmGetcheck, 0, 0)
		render := renderAuto
		if r, _, _ := procSendMessageW.Call(hRenderGPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderGPU
		} else if r, _, _ := procSendMessageW.Call(hRenderCPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderCPU
		}
		return settingsFromForm(checked == bstChecked, shareChecked == bstChecked,
			text(hMem), text(hShare), text(hFwd), text(hKey), render)
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
		cmd := exec.Command(self, "-dir", dataDir, "-recovery", action)
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
		switch msg {
		case wmCommand:
			switch wParam & 0xffff {
			case settingsSaveID:
				s, err := collect()
				diskGiB := storage.DiskGiB
				if err == nil && !portable {
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
				if err == nil && !portable && diskGiB != storage.DiskGiB {
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
			case settingsBackupID:
				launchRecovery("backup")
			case settingsRestoreID:
				launchRecovery("restore")
			case settingsResetID:
				launchRecovery("reset")
			}
			return 0
		case settingsRecoveryDone:
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
	wc := wndclassex{size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst,
		icon: icon, cursor: cursor, brush: colorBtnface + 1, className: className, iconSm: icon}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("settings: RegisterClassExW failed: %v", err)
		return false
	}

	const clientW, clientH = 480, 616
	rect := [4]int32{0, 0, clientW, clientH}
	style := uintptr(wsCaption | wsSysmenu)
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	w, hgt := rect[2]-rect[0], rect[3]-rect[1]
	sx, _, _ := procGetSystemMetrics.Call(smCxscreen)
	sy, _, _ := procGetSystemMetrics.Call(smCyscreen)
	title, _ := syscall.UTF16PtrFromString(appTitle + " settings")
	var err2 error
	hwnd, _, err2 = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		style|wsVisible, (sx-uintptr(w))/2, (sy-uintptr(hgt))/2, uintptr(w), uintptr(hgt), 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("settings: CreateWindowExW failed: %v", err2)
		return false
	}

	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	mk := func(class, label string, x, y, cx, cy int32, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		t, _ := syscall.UTF16PtrFromString(label)
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(t)),
			wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(cx), uintptr(cy), hwnd, id, hInst, 0)
		procSendMessageW.Call(h, wmSetfont, font, 1)
		return h
	}
	const left, labelW, fieldX, fieldW = 16, 150, 170, 294
	y := int32(16)
	hFull = mk("BUTTON", "Open fullscreen (Immersive)", left, y, 300, 22, bsAutocheckbox|wsTabstop, settingsFullID)
	if current.Fullscreen {
		procSendMessageW.Call(hFull, bmSetcheck, bstChecked, 0)
	}
	y += 30
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
	mk("STATIC", "Automatic tries the GPU and remembers when this PC cannot use it. GPU retries every launch.", left, y, clientW-2*left, 20, ssNoprefix, 0)
	y += 28
	mk("STATIC", "Guest memory (MiB)", left, y+3, labelW, 20, ssNoprefix, 0)
	hMem = mk("EDIT", strconv.Itoa(current.MemoryMiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsMemID)
	mk("STATIC", "0 = automatic", fieldX+112, y+3, fieldW-112, 20, ssNoprefix, 0)
	y += 34
	mk("STATIC", "Disk capacity (GiB)", left, y+3, labelW, 20, ssNoprefix, 0)
	hDisk = mk("EDIT", strconv.Itoa(storage.DiskGiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsDiskID)
	if portable {
		procEnableWindow.Call(hDisk, 0)
	}
	y += 28
	capacityHelp := "0 keeps the default. Increasing grows the disk next launch; lowering never shrinks it. Space is used as files are added."
	if portable {
		capacityHelp = "Portable disks keep their existing capacity."
	}
	mk("STATIC", capacityHelp, left, y, clientW-2*left, 36, ssNoprefix, 0)
	y += 38
	status := ""
	if !portable {
		if info, err := os.Stat(filepath.Join(dataDir, "vm", "disk.raw")); err == nil {
			status = "Current capacity: " + formatGiB(info.Size()) + ". "
		}
	}
	if available, err := diskFreeBytes(dataDir); err == nil {
		status += "Free on Windows drive: " + formatGiB(available) + "."
	}
	mk("STATIC", status, left, y, clientW-2*left, 20, ssNoprefix, 0)
	y += 26
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
	mk("STATIC", "Port forwards, one per line\n(tcp:2222:22 forwards\n127.0.0.1:2222 to sshd)", left, y+3, labelW, 60, ssNoprefix, 0)
	hFwd = mk("EDIT", strings.Join(current.Forwards, "\r\n"), fieldX, y, fieldW, 96,
		wsBorder|wsTabstop|wsVscroll|esMultiline|esAutovscroll, settingsFwdID)
	y += 106
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
	}{{"Back up...", settingsBackupID, left}, {"Restore...", settingsRestoreID, left + 150}, {"Reset guest...", settingsResetID, left + 300}} {
		button := mk("BUTTON", control.label, control.x, y, 140, 26, wsTabstop, control.id)
		if portable {
			procEnableWindow.Call(button, 0)
		}
	}
	y += 30
	help := "Close Omarchy first. Backups use saved settings. Restore creates a separate copy."
	if portable {
		help = "Backup and recovery controls are available for standard installs."
	}
	mk("STATIC", help, left, y, clientW-2*left, 34, ssNoprefix, 0)
	mk("BUTTON", "Save", clientW-16-180, clientH-40, 84, 26, bsDefpushbutton|wsTabstop, settingsSaveID)
	mk("BUTTON", "Cancel", clientW-16-84, clientH-40, 84, 26, wsTabstop, settingsCancelID)
	// Settings is often opened from the tray while the maximized QEMU window
	// owns the foreground. Raise it once, then immediately return it to the
	// normal z-order so it is visible without staying above unrelated apps.
	procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetForegroundWindow.Call(hwnd)
	procSetWindowPos.Call(hwnd, hwndNotTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetFocus.Call(hFull)

	var m msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if ok, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&m))); ok != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
	return saved
}
