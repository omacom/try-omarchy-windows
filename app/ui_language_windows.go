//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

// preferredUILanguages uses Windows display-language preferences, which can
// differ from the regional-format locale passed to the Linux guest.
func preferredUILanguages() []string {
	proc := kernel32.NewProc("GetUserPreferredUILanguages")
	var count, size uint32
	if ok, _, _ := proc.Call(0, uintptr(unsafe.Pointer(&count)), 0, uintptr(unsafe.Pointer(&size))); ok == 0 || size < 2 {
		return nil
	}
	buffer := make([]uint16, size)
	if ok, _, _ := proc.Call(0, uintptr(unsafe.Pointer(&count)), uintptr(unsafe.Pointer(&buffer[0])), uintptr(unsafe.Pointer(&size))); ok == 0 {
		return nil
	}
	var languages []string
	for start, i := 0, 0; i < len(buffer); i++ {
		if buffer[i] != 0 {
			continue
		}
		if i == start {
			break
		}
		languages = append(languages, syscall.UTF16ToString(buffer[start:i]))
		start = i + 1
	}
	return languages
}
