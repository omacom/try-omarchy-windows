package main

import (
	"syscall"
)

// hostTimeZoneKey reads the Windows time zone key name, for example
// "Eastern Standard Time", which the CLDR table maps to an IANA zone.
func hostTimeZoneKey() string {
	path, _ := syscall.UTF16PtrFromString(`SYSTEM\CurrentControlSet\Control\TimeZoneInformation`)
	var key syscall.Handle
	if syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, path, 0, syscall.KEY_READ, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	return registryString(key, "TimeZoneKeyName")
}

// hostKeyboardLayoutID reads the user's default input language as the
// eight-digit keyboard layout identifier at the head of the Preload list.
func hostKeyboardLayoutID() string {
	path, _ := syscall.UTF16PtrFromString(`Keyboard Layout\Preload`)
	var key syscall.Handle
	if syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, syscall.KEY_READ, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	return registryString(key, "1")
}

// hostLocale resolves what the guest should follow, honoring the explicit
// overrides: "" follows Windows, "keep" leaves the guest alone, anything else
// is used as given.
func hostLocale(zoneOverride, keyboardOverride string) (zone, layout, variant string) {
	switch zoneOverride {
	case "":
		zone = ianaZoneForWindows(hostTimeZoneKey())
	case "keep":
	default:
		zone = zoneOverride
	}
	switch keyboardOverride {
	case "":
		layout, variant = xkbForKLID(hostKeyboardLayoutID())
	case "keep":
	default:
		layout, variant = splitKeyboardSpec(keyboardOverride)
	}
	return zone, layout, variant
}
