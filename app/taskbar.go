//go:build windows

package main

import (
	"os"
	"syscall"
	"unsafe"
)

// The Windows 11 taskbar ignores WM_SETICON: a button's icon comes from the
// owning process's exe (QEMU's logo, for the VM window). The documented way to
// rebrand a window you don't own is the window property store: give it our
// AppUserModelID and Relaunch* properties, and the taskbar shows our icon and
// name (and pins/relaunches through TryOmarchy.exe).

var (
	ole32                           = syscall.NewLazyDLL("ole32.dll")
	procSHGetPropertyStoreForWindow = shell32.NewProc("SHGetPropertyStoreForWindow") // shell32: setup.go
	procCoInitializeEx              = ole32.NewProc("CoInitializeEx")
)

type comGUID struct {
	d1 uint32
	d2 uint16
	d3 uint16
	d4 [8]byte
}

type propertyKey struct {
	fmtid comGUID
	pid   uint32
}

type propVariant struct {
	vt         uint16
	r1, r2, r3 uint16
	val        [16]byte
}

var iidIPropertyStore = comGUID{0x886d8eeb, 0x8cf2, 0x4446, [8]byte{0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}}

// PKEY_AppUserModel_* share one fmtid; the pid selects the property.
var appUserModelFmtid = comGUID{0x9F4C2855, 0x9F79, 0x4B39, [8]byte{0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}

const (
	pidRelaunchCommand     = 2
	pidRelaunchIcon        = 3
	pidRelaunchDisplayName = 4
	pidAppUserModelID      = 5
	vtLpwstr               = 31
)

func comCall(obj uintptr, method int, args ...uintptr) uintptr {
	vtbl := *(*uintptr)(unsafe.Pointer(obj))
	fn := *(*uintptr)(unsafe.Pointer(vtbl + uintptr(method)*unsafe.Sizeof(uintptr(0))))
	all := append([]uintptr{obj}, args...)
	r, _, _ := syscall.SyscallN(fn, all...)
	return r
}

// setTaskbarIdentity brands hwnd's taskbar presence as Try Omarchy. Call once
// per window; failures are logged and harmless (worst case: QEMU's icon).
func setTaskbarIdentity(hwnd uintptr) {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	procCoInitializeEx.Call(0, 2) // apartment-threaded; "already initialized" is fine
	var ps uintptr
	if hr, _, _ := procSHGetPropertyStoreForWindow.Call(hwnd,
		uintptr(unsafe.Pointer(&iidIPropertyStore)), uintptr(unsafe.Pointer(&ps))); hr != 0 || ps == 0 {
		logf("taskbar identity: SHGetPropertyStoreForWindow hr=%#x", hr)
		return
	}
	set := func(pid uint32, s string) {
		u, _ := syscall.UTF16PtrFromString(s)
		var pv propVariant
		pv.vt = vtLpwstr
		*(*uintptr)(unsafe.Pointer(&pv.val[0])) = uintptr(unsafe.Pointer(u))
		key := propertyKey{appUserModelFmtid, pid}
		comCall(ps, 6, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&pv))) // SetValue
	}
	set(pidAppUserModelID, "SouthForge.TryOmarchy")
	set(pidRelaunchCommand, `"`+exe+`"`)
	set(pidRelaunchDisplayName, appTitle)
	set(pidRelaunchIcon, exe+",0")
	comCall(ps, 7) // Commit
	comCall(ps, 2) // Release
	logf("taskbar identity set on window %#x", hwnd)
}
