//go:build windows

package main

import (
	"fmt"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var (
	procGetDriveTypeW         = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDriveTypeW")
	procGetVolumePathNameW    = syscall.NewLazyDLL("kernel32.dll").NewProc("GetVolumePathNameW")
	procGetVolumeInformationW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetVolumeInformationW")
)

const (
	driveRemovable          = 2
	driveFixed              = 3
	driveRAMDisk            = 6
	fileSupportsSparseFiles = 0x40
)

func supportedLocalDriveType(driveType uintptr) bool {
	return driveType == driveRemovable || driveType == driveFixed || driveType == driveRAMDisk
}

func dataLocationVolumeRoot(path string) (string, error) {
	if strings.HasPrefix(filepath.VolumeName(path), `\\`) {
		return "", fmt.Errorf("network paths are not supported")
	}
	existing, err := existingDiskPath(path)
	if err != nil {
		return "", err
	}
	pathPtr, err := syscall.UTF16PtrFromString(existing)
	if err != nil {
		return "", err
	}
	var root [32768]uint16
	ok, _, callErr := procGetVolumePathNameW.Call(
		uintptr(unsafe.Pointer(pathPtr)), uintptr(unsafe.Pointer(&root[0])), uintptr(len(root)),
	)
	if ok == 0 {
		return "", callErr
	}
	return syscall.UTF16ToString(root[:]), nil
}

func dataLocationIsLocal(path string) bool {
	root, err := dataLocationVolumeRoot(path)
	if err != nil {
		return false
	}
	rootPtr, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return false
	}
	driveType, _, _ := procGetDriveTypeW.Call(uintptr(unsafe.Pointer(rootPtr)))
	return supportedLocalDriveType(driveType)
}

func dataLocationSupportsSparseFiles(path string) (bool, error) {
	root, err := dataLocationVolumeRoot(path)
	if err != nil {
		return false, err
	}
	rootPtr, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return false, err
	}
	var flags uint32
	ok, _, callErr := procGetVolumeInformationW.Call(
		uintptr(unsafe.Pointer(rootPtr)), 0, 0, 0, 0,
		uintptr(unsafe.Pointer(&flags)), 0, 0,
	)
	if ok == 0 {
		return false, callErr
	}
	return flags&fileSupportsSparseFiles != 0, nil
}

func validateStandardDataDrive(path string) error {
	if !dataLocationIsLocal(path) {
		return fmt.Errorf("choose a folder on a local Windows drive; network and unavailable drives are not supported")
	}
	supported, err := dataLocationSupportsSparseFiles(path)
	if err != nil {
		return fmt.Errorf("checking filesystem support: %w", err)
	}
	if !supported {
		return fmt.Errorf("choose an NTFS or ReFS drive; standard installs require sparse-file support")
	}
	return nil
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
			if err := validateStandardDataDrive(defaultDir); err != nil {
				errorBox("Try Omarchy cannot use the default location.\n\n" + err.Error() + "\n\nChoose another local drive or folder.")
				continue
			}
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
			if err := validateStandardDataDrive(selected); err != nil {
				errorBox("Try Omarchy cannot use that location.\n\n" + err.Error())
				continue
			}
			selectable, err := standardDataDirectorySelectable(selected)
			if err != nil {
				errorBox("Try Omarchy cannot inspect that location.\n\n" + err.Error())
				continue
			}
			if !selectable {
				errorBox("That TryOmarchy folder is not empty and is not a complete Try Omarchy installation. Choose another parent folder or an empty TryOmarchy folder.")
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
