//go:build windows

package main

import (
	"fmt"
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

// shell32 and ole32 are the package-level handles declared in setup.go and
// taskbar.go.
var (
	procSHBrowseForFolderW   = shell32.NewProc("SHBrowseForFolderW")
	procSHGetPathFromIDListW = shell32.NewProc("SHGetPathFromIDListW")
	procCoTaskMemFree        = ole32.NewProc("CoTaskMemFree")
	procIsDialogMessageW     = user32.NewProc("IsDialogMessageW")
	procLoadCursorW          = user32.NewProc("LoadCursorW")
	procGetStockObject       = syscall.NewLazyDLL("gdi32.dll").NewProc("GetStockObject")
	procSetFocus             = user32.NewProc("SetFocus")
	procAdjustWindowRectEx   = user32.NewProc("AdjustWindowRectEx")
)

const (
	wsCaption        = 0x00C00000
	wsSysmenu        = 0x00080000
	wsBorder         = 0x00800000
	wsTabstop        = 0x00010000
	wsVscroll        = 0x00200000
	esAutohscroll    = 0x0080
	esMultiline      = 0x0004
	esAutovscroll    = 0x0040
	bsAutocheckbox   = 0x0003
	bsDefpushbutton  = 0x0001
	bmGetcheck       = 0x00F0
	bmSetcheck       = 0x00F1
	bstChecked       = 1
	idcArrow         = 32512
	colorBtnface     = 15
	defaultGuiFont   = 17
	idCancel         = 2
	bifReturnOnlyFS  = 0x0001
	wmGettextlength  = 0x000E
	wmGettext        = 0x000D
	settingsSaveID   = 2001
	settingsCancelID = 2002
	settingsBrowseID = 2003
	settingsFullID   = 2010
	settingsMemID    = 2011
	settingsShareID  = 2012
	settingsFwdID    = 2013
	settingsKeyID    = 2014
)

// runSettingsDialog shows the window and returns once it closes. saved is
// true when the file was written.
func runSettingsDialog(path string) (saved bool) {
	runtime.LockOSThread()
	current, err := loadSettings(path)
	if err != nil {
		errorBox("Try Omarchy cannot read its settings:\n\n" + err.Error() + "\n\nFix or delete the file, then open the settings again.")
		return false
	}

	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var hwnd uintptr
	var hFull, hMem, hShare, hFwd, hKey uintptr

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
		var s settings
		checked, _, _ := procSendMessageW.Call(hFull, bmGetcheck, 0, 0)
		s.Fullscreen = checked == bstChecked
		mem := strings.TrimSpace(text(hMem))
		if mem != "" {
			n, err := strconv.Atoi(mem)
			if err != nil {
				return s, fmt.Errorf("guest memory must be a number of MiB, or 0 for automatic")
			}
			s.MemoryMiB = n
		}
		s.Share = strings.TrimSpace(text(hShare))
		for _, line := range strings.Split(strings.ReplaceAll(text(hFwd), "\r\n", "\n"), "\n") {
			if line = strings.TrimSpace(line); line != "" {
				s.Forwards = append(s.Forwards, line)
			}
		}
		s.SSHKey = strings.TrimSpace(text(hKey))
		if s.SSHKey != "" {
			if _, err := loadPublicKey(s.SSHKey); err != nil {
				return s, err
			}
		}
		return s, s.validate()
	}
	browseFolder := func() {
		var display [260]uint16
		title, _ := syscall.UTF16PtrFromString("Choose the Windows folder to share with Omarchy")
		type browseInfo struct {
			owner       uintptr
			root        uintptr
			displayName *uint16
			title       *uint16
			flags       uint32
			callback    uintptr
			param       uintptr
			image       int32
		}
		bi := browseInfo{owner: hwnd, displayName: &display[0], title: title, flags: bifReturnOnlyFS}
		pidl, _, _ := procSHBrowseForFolderW.Call(uintptr(unsafe.Pointer(&bi)))
		if pidl == 0 {
			return
		}
		var pathBuf [1024]uint16
		if ok, _, _ := procSHGetPathFromIDListW.Call(pidl, uintptr(unsafe.Pointer(&pathBuf[0]))); ok != 0 {
			setText(hShare, syscall.UTF16ToString(pathBuf[:]))
		}
		procCoTaskMemFree.Call(pidl)
	}

	wndProc := syscall.NewCallback(func(h, msg, wParam, lParam uintptr) uintptr {
		switch msg {
		case wmCommand:
			switch wParam & 0xffff {
			case settingsSaveID:
				s, err := collect()
				if err == nil {
					err = saveSettings(path, s)
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
			}
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

	const clientW, clientH = 480, 372
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
	y += 34
	mk("STATIC", "Guest memory (MiB, 0 = automatic)", left, y+3, labelW, 20, ssNoprefix, 0)
	hMem = mk("EDIT", strconv.Itoa(current.MemoryMiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsMemID)
	y += 34
	mk("STATIC", "Shared folder", left, y+3, labelW, 20, ssNoprefix, 0)
	hShare = mk("EDIT", current.Share, fieldX, y, fieldW-80, 24, wsBorder|wsTabstop|esAutohscroll, settingsShareID)
	mk("BUTTON", "Browse...", fieldX+fieldW-72, y, 72, 24, wsTabstop, settingsBrowseID)
	y += 34
	mk("STATIC", "Port forwards, one per line\n(tcp:2222:22 forwards\n127.0.0.1:2222 to sshd)", left, y+3, labelW, 60, ssNoprefix, 0)
	hFwd = mk("EDIT", strings.Join(current.Forwards, "\r\n"), fieldX, y, fieldW, 96,
		wsBorder|wsTabstop|wsVscroll|esMultiline|esAutovscroll, settingsFwdID)
	y += 106
	mk("STATIC", "SSH public key file\n(blank: your ~/.ssh/id_*.pub)", left, y+3, labelW, 40, ssNoprefix, 0)
	hKey = mk("EDIT", current.SSHKey, fieldX, y, fieldW, 24, wsBorder|wsTabstop|esAutohscroll, settingsKeyID)
	y += 36
	mk("STATIC", "Saved to settings.json and used on the next launch. A flag on the command line wins for that launch.",
		left, y, clientW-2*left, 36, ssNoprefix, 0)
	mk("BUTTON", "Save", clientW-16-180, clientH-40, 84, 26, bsDefpushbutton|wsTabstop, settingsSaveID)
	mk("BUTTON", "Cancel", clientW-16-84, clientH-40, 84, 26, wsTabstop, settingsCancelID)
	procSetForegroundWindow.Call(hwnd)
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
