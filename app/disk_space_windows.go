//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

var (
	procGetDiskFreeSpaceExW    = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDiskFreeSpaceExW")
	procGetCompressedFileSizeW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetCompressedFileSizeW")
	procSetLastError           = syscall.NewLazyDLL("kernel32.dll").NewProc("SetLastError")
)

func existingDiskPath(path string) (string, error) {
	path = filepath.Clean(path)
	for {
		if _, err := os.Stat(path); err == nil {
			return path, nil
		} else if !os.IsNotExist(err) {
			return "", err
		}
		parent := filepath.Dir(path)
		if parent == path {
			return "", fmt.Errorf("no existing parent for %s", path)
		}
		path = parent
	}
}

func platformDiskFreeBytes(path string) (int64, error) {
	existing, err := existingDiskPath(path)
	if err != nil {
		return 0, err
	}
	ptr, err := syscall.UTF16PtrFromString(existing)
	if err != nil {
		return 0, err
	}
	var available uint64
	r1, _, callErr := procGetDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(ptr)), uintptr(unsafe.Pointer(&available)), 0, 0)
	if r1 == 0 {
		return 0, callErr
	}
	if available > uint64(1<<63-1) {
		return 1<<63 - 1, nil
	}
	return int64(available), nil
}

func platformAllocatedFileBytes(path string) (int64, error) {
	ptr, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	var high uint32
	procSetLastError.Call(0)
	low, _, callErr := procGetCompressedFileSizeW.Call(uintptr(unsafe.Pointer(ptr)), uintptr(unsafe.Pointer(&high)))
	if uint32(low) == 0xffffffff && callErr != syscall.Errno(0) {
		return 0, callErr
	}
	allocated := uint64(high)<<32 | uint64(uint32(low))
	if allocated > uint64(1<<63-1) {
		return 0, fmt.Errorf("allocated file size is too large")
	}
	return int64(allocated), nil
}
