//go:build windows

package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

type recoveryGUID struct {
	A    uint32
	B, C uint16
	D    [8]byte
}

var recoveryOpenClass = recoveryGUID{0xdc1c5a9c, 0xe88a, 0x4dde, [8]byte{0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}}
var recoveryOpenIID = recoveryGUID{0xd57c7288, 0xd4ad, 0x4768, [8]byte{0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}}
var recoverySaveClass = recoveryGUID{0xc0b4e2f3, 0xba21, 0x4773, [8]byte{0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b}}
var recoverySaveIID = recoveryGUID{0x84bccd23, 0x5fde, 0x4cdb, [8]byte{0xae, 0xa4, 0xaf, 0x64, 0xb8, 0x3d, 0x78, 0xab}}

//go:uintptrescapes
func recoveryCOMCall(object uintptr, index int, args ...uintptr) uintptr {
	table := *(*uintptr)(unsafe.Pointer(object))
	method := *(*uintptr)(unsafe.Pointer(table + uintptr(index)*unsafe.Sizeof(uintptr(0))))
	result, _, _ := syscall.SyscallN(method, append([]uintptr{object}, args...)...)
	return result
}
func recoveryCOMError(result uintptr) error {
	if int32(result) < 0 {
		return fmt.Errorf("Windows file picker failed (0x%08x)", uint32(result))
	}
	return nil
}

// Common Item Dialog uses the standard Windows file browser, including current
// locations, search, keyboard navigation, and accessible system controls.
func chooseRecoveryPath(owner uintptr, title, filename string, save, folder bool) (string, bool, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	init, _, _ := ole32.NewProc("CoInitializeEx").Call(0, 2)
	if err := recoveryCOMError(init); err != nil {
		return "", false, err
	}
	defer ole32.NewProc("CoUninitialize").Call()
	class, iid := recoveryOpenClass, recoveryOpenIID
	if save {
		class, iid = recoverySaveClass, recoverySaveIID
	}
	var dialog uintptr
	hr, _, _ := ole32.NewProc("CoCreateInstance").Call(uintptr(unsafe.Pointer(&class)), 0, 1, uintptr(unsafe.Pointer(&iid)), uintptr(unsafe.Pointer(&dialog)))
	if err := recoveryCOMError(hr); err != nil {
		return "", false, err
	}
	defer recoveryCOMCall(dialog, 2)
	var options uint32
	if err := recoveryCOMError(recoveryCOMCall(dialog, 10, uintptr(unsafe.Pointer(&options)))); err != nil {
		return "", false, err
	}
	options |= 0x40 | 0x800 | 0x02000000 // filesystem, existing parent, no recent-items entry
	if save {
		options &^= 0x2
	} // Existing backups are never overwritten.
	if folder {
		options |= 0x20
	} else if !save {
		options |= 0x1000
	}
	if err := recoveryCOMError(recoveryCOMCall(dialog, 9, uintptr(options))); err != nil {
		return "", false, err
	}
	label, err := syscall.UTF16PtrFromString(title)
	if err != nil {
		return "", false, err
	}
	if err = recoveryCOMError(recoveryCOMCall(dialog, 17, uintptr(unsafe.Pointer(label)))); err != nil {
		return "", false, err
	}
	if filename != "" {
		name, err := syscall.UTF16PtrFromString(filename)
		if err != nil {
			return "", false, err
		}
		if err = recoveryCOMError(recoveryCOMCall(dialog, 15, uintptr(unsafe.Pointer(name)))); err != nil {
			return "", false, err
		}
	}
	if !folder {
		label, _ := syscall.UTF16PtrFromString("Omarchy backups (*.zip)")
		pattern, _ := syscall.UTF16PtrFromString("*.zip")
		filter := struct{ label, pattern *uint16 }{label, pattern}
		if err = recoveryCOMError(recoveryCOMCall(dialog, 4, 1, uintptr(unsafe.Pointer(&filter)))); err != nil {
			return "", false, err
		}
		ext, _ := syscall.UTF16PtrFromString("zip")
		if err = recoveryCOMError(recoveryCOMCall(dialog, 22, uintptr(unsafe.Pointer(ext)))); err != nil {
			return "", false, err
		}
	}
	hr = recoveryCOMCall(dialog, 3, owner)
	if uint32(hr) == 0x800704c7 {
		return "", false, nil
	}
	if err = recoveryCOMError(hr); err != nil {
		return "", false, err
	}
	var item uintptr
	if err = recoveryCOMError(recoveryCOMCall(dialog, 20, uintptr(unsafe.Pointer(&item)))); err != nil {
		return "", false, err
	}
	defer recoveryCOMCall(item, 2)
	var path *uint16
	if err = recoveryCOMError(recoveryCOMCall(item, 5, 0x80058000, uintptr(unsafe.Pointer(&path)))); err != nil {
		return "", false, err
	}
	defer procCoTaskMemFree.Call(uintptr(unsafe.Pointer(path)))
	// Shell item paths are terminated UTF-16 strings owned by COM.
	var chars []uint16
	for offset := uintptr(0); offset < 32768; offset++ {
		c := *(*uint16)(unsafe.Add(unsafe.Pointer(path), offset*2))
		if c == 0 {
			return syscall.UTF16ToString(chars), true, nil
		}
		chars = append(chars, c)
	}
	return "", false, fmt.Errorf("selected path is too long")
}
