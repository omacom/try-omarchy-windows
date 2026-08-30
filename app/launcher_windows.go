//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var procMoveFileExW = kernel32.NewProc("MoveFileExW")

const (
	moveFileReplaceExisting = 0x1
	moveFileWriteThrough    = 0x8
)

func replaceLauncher(staged, target string) error {
	from, err := syscall.UTF16PtrFromString(staged)
	if err != nil {
		return err
	}
	to, err := syscall.UTF16PtrFromString(target)
	if err != nil {
		return err
	}
	r, _, callErr := procMoveFileExW.Call(
		uintptr(unsafe.Pointer(from)), uintptr(unsafe.Pointer(to)),
		moveFileReplaceExisting|moveFileWriteThrough,
	)
	if r == 0 {
		return callErr
	}
	return nil
}

func stableLauncherPath(dir string) (string, error) {
	self, err := os.Executable()
	if err != nil {
		return "", err
	}
	target := filepath.Join(dir, stableLauncherName)
	if err := copyLauncher(self, target, replaceLauncher); err != nil {
		return "", err
	}
	return target, nil
}

func shortcutArguments(dir string) string {
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), "TryOmarchy")
	if pathsEqual(dir, defaultDir) {
		return ""
	}
	// Double quotes cannot occur in a Windows path. Refuse to create a broken
	// shortcut if a synthetic command-line value somehow contains one.
	if strings.ContainsRune(dir, '"') {
		return ""
	}
	return `-dir "` + dir + `"`
}

func createLauncherShortcuts(target, dir string, startMenu, desktop bool) error {
	if !startMenu && !desktop {
		return nil
	}
	const script = `$ErrorActionPreference='Stop'; ` +
		`$shell=New-Object -ComObject WScript.Shell; ` +
		`function Add-TryOmarchyShortcut([string]$path) { ` +
		`$shortcut=$shell.CreateShortcut($path); ` +
		`$shortcut.TargetPath=$env:TRYOMARCHY_SHORTCUT_TARGET; ` +
		`$shortcut.Arguments=$env:TRYOMARCHY_SHORTCUT_ARGS; ` +
		`$shortcut.WorkingDirectory=$env:TRYOMARCHY_SHORTCUT_WORKDIR; ` +
		`$shortcut.IconLocation=$env:TRYOMARCHY_SHORTCUT_TARGET+',0'; ` +
		`$shortcut.Description='Run Omarchy on Windows'; $shortcut.Save() }; ` +
		`if ($env:TRYOMARCHY_SHORTCUT_START -eq '1') { ` +
		`Add-TryOmarchyShortcut (Join-Path ([Environment]::GetFolderPath('Programs')) 'Try Omarchy.lnk') }; ` +
		`if ($env:TRYOMARCHY_SHORTCUT_DESKTOP -eq '1') { ` +
		`Add-TryOmarchyShortcut (Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Try Omarchy.lnk') }`
	cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"),
		"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script)
	cmd.Env = append(os.Environ(),
		"TRYOMARCHY_SHORTCUT_TARGET="+target,
		"TRYOMARCHY_SHORTCUT_ARGS="+shortcutArguments(dir),
		"TRYOMARCHY_SHORTCUT_WORKDIR="+dir,
		fmt.Sprintf("TRYOMARCHY_SHORTCUT_START=%d", boolInt(startMenu)),
		fmt.Sprintf("TRYOMARCHY_SHORTCUT_DESKTOP=%d", boolInt(desktop)),
	)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("creating shortcuts: %w: %s", err, strings.TrimSpace(string(output)))
	}
	return nil
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}

// offerLauncherShortcuts runs only after the guest and writable disk are
// complete. The signed launcher is copied into the app-data folder on every
// successful launch, so opening a newer downloaded release refreshes the
// stable target without making existing shortcuts fragile.
func offerLauncherShortcuts(dir string) {
	installDir, err := filepath.Abs(dir)
	if err != nil {
		logf("stable launcher path: %v", err)
		return
	}
	target, err := stableLauncherPath(installDir)
	if err != nil {
		logf("stable launcher: %v", err)
		return
	}
	if shortcutOfferRecorded(installDir) {
		return
	}
	startMenu := msgBox("Add Try Omarchy to the Start menu?", mbYesNo|mbIconQuestion) == idYes
	desktop := msgBox("Add a Try Omarchy shortcut to your desktop?", mbYesNo|mbIconQuestion) == idYes
	if err := createLauncherShortcuts(target, installDir, startMenu, desktop); err != nil {
		logf("shortcuts: %v", err)
		errorBox("Try Omarchy is ready, but Windows could not create the requested shortcut. You can keep using the downloaded launcher.\n\n" + err.Error())
		return
	}
	if err := recordShortcutOffer(installDir); err != nil {
		logf("shortcut offer marker: %v", err)
	}
}
