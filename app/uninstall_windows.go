package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var (
	procRegCreateKeyExW = advapi32.NewProc("RegCreateKeyExW")
	procRegSetValueExW  = advapi32.NewProc("RegSetValueExW")
	procRegDeleteTreeW  = advapi32.NewProc("RegDeleteTreeW")
)

const (
	regOptionNonVolatile = 0
	regSz                = 1
	regDword             = 4
)

func defaultDataDirectory() string {
	return filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
}

// registerUninstallEntry lists this installation under Apps & features. It
// runs on every normal launch, so an entry a user removed by hand comes back
// only while the install still exists, and the recorded launcher path follows
// the stable copy.
func registerUninstallEntry(target, dir string) error {
	defaultDir := defaultDataDirectory()
	keyPath, _ := syscall.UTF16PtrFromString(uninstallRegistryParent + `\` + uninstallKeyName(dir, defaultDir))
	var key syscall.Handle
	var disposition uint32
	r, _, err := procRegCreateKeyExW.Call(uintptr(syscall.HKEY_CURRENT_USER), uintptr(unsafe.Pointer(keyPath)), 0, 0,
		regOptionNonVolatile, uintptr(syscall.KEY_WRITE), 0, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&disposition)))
	if r != 0 {
		return fmt.Errorf("creating the Apps & features entry: %v", err)
	}
	defer syscall.RegCloseKey(key)
	values := map[string]string{
		"DisplayName":     uninstallDisplayName(dir, defaultDir),
		"DisplayVersion":  displayVersion(currentVersion),
		"Publisher":       "Omacom",
		"InstallLocation": dir,
		"DisplayIcon":     target + ",0",
		"UninstallString": uninstallCommand(target, dir),
		"URLInfoAbout":    "https://github.com/omacom/try-omarchy-windows",
		"InstallDate":     time.Now().Format("20060102"),
	}
	for name, value := range values {
		if err := regSetString(key, name, value); err != nil {
			return err
		}
	}
	for _, name := range []string{"NoModify", "NoRepair"} {
		if err := regSetDword(key, name, 1); err != nil {
			return err
		}
	}
	return nil
}

func unregisterUninstallEntry(dir string) error {
	parent, _ := syscall.UTF16PtrFromString(uninstallRegistryParent)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, parent, 0, syscall.KEY_WRITE|syscall.KEY_READ, &key); err != nil {
		return nil
	}
	defer syscall.RegCloseKey(key)
	name, _ := syscall.UTF16PtrFromString(uninstallKeyName(dir, defaultDataDirectory()))
	r, _, err := procRegDeleteTreeW.Call(uintptr(key), uintptr(unsafe.Pointer(name)))
	if r != 0 && r != uintptr(syscall.ERROR_FILE_NOT_FOUND) {
		return fmt.Errorf("removing the Apps & features entry: %v", err)
	}
	return nil
}

func regSetString(key syscall.Handle, name, value string) error {
	n, _ := syscall.UTF16PtrFromString(name)
	v, _ := syscall.UTF16FromString(value)
	r, _, err := procRegSetValueExW.Call(uintptr(key), uintptr(unsafe.Pointer(n)), 0, regSz,
		uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)*2))
	if r != 0 {
		return fmt.Errorf("writing %s: %v", name, err)
	}
	return nil
}

func regSetDword(key syscall.Handle, name string, value uint32) error {
	n, _ := syscall.UTF16PtrFromString(name)
	r, _, err := procRegSetValueExW.Call(uintptr(key), uintptr(unsafe.Pointer(n)), 0, regDword,
		uintptr(unsafe.Pointer(&value)), 4)
	if r != 0 {
		return fmt.Errorf("writing %s: %v", name, err)
	}
	return nil
}

// removeLauncherShortcuts deletes the Start menu and Desktop shortcuts that
// point at this installation's launcher and leaves any other install's alone.
func removeLauncherShortcuts(target string) error {
	const script = `$ErrorActionPreference='Stop'; $shell=New-Object -ComObject WScript.Shell; ` +
		`$programs=[Environment]::GetFolderPath('Programs'); $desktop=[Environment]::GetFolderPath('DesktopDirectory'); ` +
		`foreach($path in @((Join-Path $programs 'Try Omarchy.lnk'),(Join-Path $programs 'Try Omarchy Settings.lnk'),(Join-Path $desktop 'Try Omarchy.lnk'))) { ` +
		`if (Test-Path -LiteralPath $path) { $link=$shell.CreateShortcut($path); ` +
		`if ([StringComparer]::OrdinalIgnoreCase.Equals($link.TargetPath,$env:TRYOMARCHY_SHORTCUT_TARGET)) { Remove-Item -LiteralPath $path -Force } } }`
	cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"),
		"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script)
	cmd.Env = append(os.Environ(), "TRYOMARCHY_SHORTCUT_TARGET="+target)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("removing shortcuts: %w: %s", err, strings.TrimSpace(string(output)))
	}
	return nil
}

// runUninstall removes a stopped standard installation: an optional backup
// first, then shortcuts, the Apps & features entry, the data-location
// pointer, and the data folder itself.
func runUninstall(dir string) error {
	dir, err := filepath.Abs(dir)
	if err != nil {
		return err
	}
	if disk := filepath.Join(dir, "vm", "disk.raw"); fileExists(disk) {
		f, err := openBackupDisk(disk)
		if err != nil {
			return fmt.Errorf("close Try Omarchy before removing it: %w", err)
		}
		f.Close()
	}
	choice := msgBox("Remove Try Omarchy from this PC?\n\nThis deletes the Omarchy virtual disk and everything inside it, the downloaded image and runtime, settings, and the launcher in:\n\n"+dir+"\n\nShortcuts and the Apps & features entry are removed. Windows shared folders and the original download are kept.\n\nCreate a full backup first?\nYes: choose a backup. No: skip the backup. Cancel: keep everything.", mbYesNoCancel|mbIconQuestion|mbDefbutton2)
	if choice != idYes && choice != idNo {
		return nil
	}
	if choice == idYes {
		name, ok, err := chooseBackupDestination()
		if err != nil || !ok {
			return err
		}
		beginRecoveryProgress("Backing up before removal...")
		err = writeVMBackupProgress(dir, name, recoveryProgress("Backing up"))
		uiDone()
		if err != nil {
			return err
		}
	}
	if msgBox("Remove Try Omarchy and delete "+dir+" now?", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return nil
	}
	target := filepath.Join(dir, stableLauncherName)
	if err := removeLauncherShortcuts(target); err != nil {
		logf("uninstall: %v", err)
	}
	if err := unregisterUninstallEntry(dir); err != nil {
		logf("uninstall: %v", err)
	}
	defaultDir := defaultDataDirectory()
	if pointed, ok, _ := loadDataLocationPointer(defaultDir); ok && pathsEqual(pointed, dir) {
		os.Remove(dataLocationPointerPath(defaultDir))
		os.Remove(defaultDir)
	}
	self, err := os.Executable()
	if err != nil {
		return err
	}
	if rel, err := filepath.Rel(dir, self); err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		// This process runs from inside the folder it must delete. A copy
		// outside the folder finishes the job after this one exits.
		helper := filepath.Join(os.TempDir(), fmt.Sprintf("TryOmarchy-uninstall-%d.exe", os.Getpid()))
		if err := copyLauncher(self, helper, replaceLauncher); err != nil {
			return fmt.Errorf("preparing the removal helper: %w", err)
		}
		cmd := exec.Command(helper, "-dir", dir, "-uninstall-finish", "-update-wait-pid", strconv.Itoa(os.Getpid()))
		if err := cmd.Start(); err != nil {
			return fmt.Errorf("starting the removal helper: %w", err)
		}
		return nil
	}
	if err := removeAllWithRetry(dir); err != nil {
		return err
	}
	infoBox("Try Omarchy was removed.")
	return nil
}

// finishUninstall runs from the temporary helper copy: wait for the launcher
// that started it, delete the folder, report, then schedule its own removal.
func finishUninstall(dir string, waitPID int) int {
	if waitPID > 0 {
		waitForProcess(waitPID)
	}
	dir, err := filepath.Abs(dir)
	if err == nil {
		err = removeAllWithRetry(dir)
	}
	code := 0
	if err != nil {
		errorBox("Try Omarchy could not delete its folder:\n\n" + dir + "\n\n" + err.Error() + "\n\nDelete it by hand to finish removing Try Omarchy.")
		code = 1
	} else {
		infoBox("Try Omarchy was removed.")
	}
	// Started after the message box closes, so the helper file is no longer
	// in use by the time cmd deletes it.
	self, _ := os.Executable()
	cleanup := exec.Command(system32("cmd.exe"))
	// Go would escape the inner quotes for cmd, which does not understand
	// backslash escapes. Hand cmd the exact line; /s strips the outer quotes.
	cleanup.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow,
		CmdLine: `"` + system32("cmd.exe") + `" /d /s /c "ping -n 4 127.0.0.1 >nul & del /q "` + self + `""`}
	_ = cleanup.Start()
	return code
}

func fileExists(path string) bool {
	info, err := os.Lstat(path)
	return err == nil && info.Mode().IsRegular()
}
