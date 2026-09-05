package main

import (
	"sort"
	"strings"
	"syscall"
	"unsafe"
)

// displayDriverIdentity summarizes the installed display drivers from the
// display adapter class key, so a driver update invalidates a remembered CPU
// rendering result. It never fails the launch: an unreadable registry yields
// an empty identity, which the probe treats as unknown.
func displayDriverIdentity() string {
	const classKey = `SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}`
	var key syscall.Handle
	path, _ := syscall.UTF16PtrFromString(classKey)
	if syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, path, 0, syscall.KEY_READ|syscall.KEY_ENUMERATE_SUB_KEYS, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	var entries []string
	for index := uint32(0); index < 64; index++ {
		name := make([]uint16, 256)
		nameLen := uint32(len(name))
		if syscall.RegEnumKeyEx(key, index, &name[0], &nameLen, nil, nil, nil, nil) != nil {
			break
		}
		var sub syscall.Handle
		if syscall.RegOpenKeyEx(key, &name[0], 0, syscall.KEY_READ, &sub) != nil {
			continue
		}
		desc := registryString(sub, "DriverDesc")
		version := registryString(sub, "DriverVersion")
		syscall.RegCloseKey(sub)
		if desc == "" && version == "" {
			continue
		}
		entries = append(entries, desc+"="+version)
	}
	sort.Strings(entries)
	return strings.Join(entries, ";")
}

func registryString(key syscall.Handle, name string) string {
	valueName, _ := syscall.UTF16PtrFromString(name)
	var valueType, size uint32
	if syscall.RegQueryValueEx(key, valueName, nil, &valueType, nil, &size) != nil || size == 0 || size > 4096 || valueType != syscall.REG_SZ {
		return ""
	}
	buf := make([]uint16, size/2+1)
	if syscall.RegQueryValueEx(key, valueName, nil, &valueType, (*byte)(unsafe.Pointer(&buf[0])), &size) != nil {
		return ""
	}
	return strings.TrimSpace(syscall.UTF16ToString(buf))
}
