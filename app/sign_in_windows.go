//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// Use a per-install link in the current user's Startup folder. Windows runs
// it after sign-in, with no administrator privilege or machine-wide change.
func signInShortcutPath(dir string) (string, error) {
	buffer := make([]uint16, 260)
	hr, _, _ := shell32.NewProc("SHGetFolderPathW").Call(0, 7, 0, 0, uintptr(unsafe.Pointer(&buffer[0]))) // CSIDL_STARTUP
	if int32(hr) < 0 {
		return "", fmt.Errorf("locating the Windows Startup folder: HRESULT 0x%08x", uint32(hr))
	}
	folder := syscall.UTF16ToString(buffer)
	if folder == "" {
		return "", fmt.Errorf("Windows returned an empty Startup folder")
	}
	name := strings.TrimPrefix(uninstallKeyName(dir, defaultDataDirectory()), "TryOmarchy")
	return filepath.Join(folder, "Try Omarchy"+name+".lnk"), nil
}

func syncSignInShortcut(target, dir string, enabled bool) error {
	path, err := signInShortcutPath(dir)
	if err != nil {
		return err
	}
	return syncSignInShortcutAt(path, target, dir, enabled)
}

func syncSignInShortcutAt(path, target, dir string, enabled bool) error {
	if _, err := os.Lstat(path); err == nil {
		owned, _, err := readShellLink(path)
		if err != nil {
			return fmt.Errorf("checking sign-in shortcut: %w", err)
		}
		if !sameShortcutTarget(owned, target) {
			return fmt.Errorf("sign-in shortcut %q belongs to another installation", path)
		}
	} else if !os.IsNotExist(err) {
		return err
	} else if !enabled {
		return nil
	}
	if !enabled {
		return os.Remove(path)
	}
	if _, err := os.Stat(target); err != nil {
		return fmt.Errorf("sign-in launcher is unavailable: %w", err)
	}
	return writeShellLink(path, target, launchShortcutArguments(dir, true), dir)
}
