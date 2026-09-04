//go:build windows

package main

import (
	"fmt"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var procGetDriveTypeW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDriveTypeW")

const (
	driveRemovable = 2
	driveFixed     = 3
	driveRAMDisk   = 6
)

func supportedLocalDriveType(driveType uintptr) bool {
	return driveType == driveRemovable || driveType == driveFixed || driveType == driveRAMDisk
}

func dataLocationIsLocal(path string) bool {
	volume := filepath.VolumeName(path)
	if volume == "" || strings.HasPrefix(volume, `\\`) {
		return false
	}
	root, err := syscall.UTF16PtrFromString(volume + `\`)
	if err != nil {
		return false
	}
	driveType, _, _ := procGetDriveTypeW.Call(uintptr(unsafe.Pointer(root)))
	return supportedLocalDriveType(driveType)
}

// chooseFirstRunDataDirectory asks once, before any payload is downloaded.
// The folder picker selects a drive or parent folder and the launcher keeps
// its files together in a TryOmarchy child directory.
func chooseFirstRunDataDirectory(defaultDir string) (string, bool, error) {
	for {
		answer := msgBox(
			"Choose where Try Omarchy stores its virtual machine, graphics runtime, and downloads.\n\n"+
				"Default location:\n"+defaultDir+"\n\n"+
				"Choose Yes to select another local drive or folder. Choose No to use the default location.",
			mbYesNoCancel|mbIconQuestion,
		)
		switch answer {
		case idCancel:
			return "", false, nil
		case idNo:
			return defaultDir, true, nil
		case idYes:
			parent, ok := browseForFolder(0, "Choose a local drive or parent folder. Try Omarchy will create a TryOmarchy folder inside it.")
			if !ok {
				continue
			}
			selected, err := dataDirectoryForSelection(parent)
			if err != nil {
				errorBox("Try Omarchy cannot use that location.\n\n" + err.Error())
				continue
			}
			if !dataLocationIsLocal(selected) {
				errorBox("Choose a folder on a local Windows drive. Network locations are not supported for the virtual disk.")
				continue
			}
			if err := ensureDataDirectoryWritable(selected); err != nil {
				errorBox("Try Omarchy cannot write to that location.\n\n" + err.Error())
				continue
			}
			available, err := diskFreeBytes(selected)
			if err != nil {
				errorBox("Try Omarchy cannot check the free space at that location.\n\n" + err.Error())
				continue
			}
			if msgBox(fmt.Sprintf("Store Try Omarchy here?\n\n%s\n\nAvailable space: %s", selected, formatGiB(available)), mbYesNo|mbIconQuestion) != idYes {
				continue
			}
			return selected, true, nil
		default:
			return "", false, nil
		}
	}
}
