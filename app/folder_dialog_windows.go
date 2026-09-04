//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var (
	procSHBrowseForFolderW   = shell32.NewProc("SHBrowseForFolderW")
	procSHGetPathFromIDListW = shell32.NewProc("SHGetPathFromIDListW")
	procCoTaskMemFree        = ole32.NewProc("CoTaskMemFree")
)

const bifReturnOnlyFS = 0x0001

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

func browseForFolder(owner uintptr, prompt string) (string, bool) {
	var display [260]uint16
	title, _ := syscall.UTF16PtrFromString(prompt)
	bi := browseInfo{owner: owner, displayName: &display[0], title: title, flags: bifReturnOnlyFS}
	pidl, _, _ := procSHBrowseForFolderW.Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return "", false
	}
	defer procCoTaskMemFree.Call(pidl)
	var pathBuf [32768]uint16
	if ok, _, _ := procSHGetPathFromIDListW.Call(pidl, uintptr(unsafe.Pointer(&pathBuf[0]))); ok != 0 {
		return syscall.UTF16ToString(pathBuf[:]), true
	}
	return "", false
}
