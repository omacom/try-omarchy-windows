//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

// Windows draws the frame around QEMU's SDL window, and it stays light unless
// the window opts into dark mode. The title enforcer follows the user's app
// theme (Settings > Personalization > Colors) so the title bar matches the
// taskbar and the Omarchy desktop, including after a change while running.
const (
	dwmwaUseImmersiveDarkMode = 20
	swpNoZorder               = 0x0004
	swpNoActivate             = 0x0010
	swpFrameChanged           = 0x0020
)

func windowsAppsUseDarkTheme() bool {
	var key syscall.Handle
	path, _ := syscall.UTF16PtrFromString(`Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`)
	if syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, syscall.KEY_READ, &key) != nil {
		return false
	}
	defer syscall.RegCloseKey(key)
	name, _ := syscall.UTF16PtrFromString("AppsUseLightTheme")
	var value, valueType uint32
	size := uint32(unsafe.Sizeof(value))
	if syscall.RegQueryValueEx(key, name, nil, &valueType, (*byte)(unsafe.Pointer(&value)), &size) != nil || valueType != syscall.REG_DWORD {
		return false
	}
	return value == 0
}

// setDarkTitleBar returns false when DWM refuses the attribute, as Windows 10
// builds before 20H1 do.
func setDarkTitleBar(hwnd uintptr, dark bool) bool {
	value := int32(0)
	if dark {
		value = 1
	}
	if hr, _, _ := procDwmSetWindowAttr.Call(hwnd, dwmwaUseImmersiveDarkMode, uintptr(unsafe.Pointer(&value)), unsafe.Sizeof(value)); hr != 0 {
		return false
	}
	// DWM uses the new colors at the frame's next non-client paint.
	procSetWindowPos.Call(hwnd, 0, 0, 0, 0, 0, swpNoMove|swpNoSize|swpNoZorder|swpNoActivate|swpFrameChanged)
	return true
}
