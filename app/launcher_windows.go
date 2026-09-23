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

// Keep first-run location selection single-instance without occupying the VM
// lifecycle port needed by recovery. Windows releases the object on process exit,
// so a crashed launcher cannot leave a stale lock file behind.
func acquireLauncherMenu(name string) (uintptr, error) {
	label, err := syscall.UTF16PtrFromString(`Local\` + name + "-LauncherMenu")
	if err != nil {
		return 0, err
	}
	handle, _, callErr := kernel32.NewProc("CreateMutexW").Call(0, 0, uintptr(unsafe.Pointer(label)))
	if handle == 0 {
		return 0, fmt.Errorf("opening launcher lock: %w", callErr)
	}
	if callErr == syscall.Errno(183) { // ERROR_ALREADY_EXISTS
		procCloseHandle.Call(handle)
		return 0, nil
	}
	return handle, nil
}

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
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
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

func launchShortcutArguments(dir string, startAutomatically bool) string {
	args := shortcutArguments(dir)
	if startAutomatically {
		args = strings.TrimSpace(args + " -start")
	}
	return args
}

func settingsShortcutArguments(dir string) string {
	return strings.TrimSpace(shortcutArguments(dir) + " -settings")
}

func writeLauncherShortcuts(paths []string, target, dir string, startMenu, desktop, startAutomatically bool) error {
	// Check every selected path before writing any of them. A second install
	// must not take over the existing install's Start Menu or Desktop links.
	for i, path := range paths {
		if i < 2 && !startMenu || i == 2 && !desktop {
			continue
		}
		if _, err := os.Lstat(path); os.IsNotExist(err) {
			continue
		} else if err != nil {
			return err
		}
		ownedTarget, _, err := readShellLink(path)
		if err != nil {
			return fmt.Errorf("checking Windows shortcut %q: %w", path, err)
		}
		if !sameShortcutTarget(ownedTarget, target) {
			return fmt.Errorf("Windows shortcut %q already belongs to another installation", path)
		}
	}
	for i, path := range paths {
		if i < 2 && !startMenu || i == 2 && !desktop {
			continue
		}
		args := launchShortcutArguments(dir, startAutomatically)
		if i == 1 {
			args = settingsShortcutArguments(dir)
		}
		if err := writeShellLink(path, target, args, dir); err != nil {
			return err
		}
	}
	return nil
}

// A second installation cannot take over another copy's global shortcuts.
// Put launchers beside its own data instead, and keep both links together.
func writeFolderLaunchers(target, dir string, startAutomatically bool) error {
	items := []struct{ path, arguments string }{
		{filepath.Join(dir, "Start Omarchy.lnk"), launchShortcutArguments(dir, startAutomatically)},
		{filepath.Join(dir, "Settings.lnk"), settingsShortcutArguments(dir)},
	}
	for _, item := range items {
		if _, err := os.Lstat(item.path); os.IsNotExist(err) {
			continue
		} else if err != nil {
			return err
		}
		ownedTarget, _, err := readShellLink(item.path)
		if err != nil {
			return fmt.Errorf("checking folder shortcut %q: %w", item.path, err)
		}
		if !sameShortcutTarget(ownedTarget, target) {
			return fmt.Errorf("folder shortcut %q already belongs to another installation", item.path)
		}
	}
	for _, item := range items {
		if err := writeShellLink(item.path, target, item.arguments, dir); err != nil {
			return err
		}
	}
	return nil
}

// Record the first-run choice even when Windows cannot create a shortcut.
// Otherwise a second installation repeats the same prompt on every launch.
func finishLauncherShortcutChoice(paths []string, target, dir string, startMenu, desktop, startAutomatically bool) (string, error) {
	var globalErr, folderErr error
	if startMenu || desktop {
		globalErr = writeLauncherShortcuts(paths, target, dir, startMenu, desktop, startAutomatically)
		if globalErr != nil {
			folderErr = writeFolderLaunchers(target, dir, startAutomatically)
		}
	}
	if err := recordShortcutOffer(dir); err != nil {
		return "", fmt.Errorf("saving shortcut choice: %w", err)
	}
	if folderErr != nil {
		return "", fmt.Errorf("Windows shortcut: %v; folder shortcuts: %w", globalErr, folderErr)
	}
	if globalErr != nil {
		return "Windows could not use the selected shortcut because " + globalErr.Error() +
			"\n\nOpen Start Omarchy or Settings beside this installation. Your existing Windows shortcuts were not changed.", nil
	}
	return "", nil
}

func updateLaunchShortcuts(target, dir string, startAutomatically bool) error {
	paths, err := launcherShortcutPaths()
	if err != nil {
		return err
	}
	paths = []string{paths[0], paths[2], filepath.Join(dir, "Start Omarchy.lnk")}
	return changeOwnedShortcuts(paths, []string{target}, func(path, _ string) error {
		return writeShellLink(path, target, launchShortcutArguments(dir, startAutomatically), dir)
	})
}

func ensureSettingsShortcutForExistingInstall(target, dir string) error {
	paths, err := launcherShortcutPaths()
	if err != nil {
		return err
	}
	return changeOwnedShortcuts(paths[:1], []string{target}, func(_, _ string) error {
		return writeShellLink(paths[1], target, settingsShortcutArguments(dir), dir)
	})
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}

func chooseProvisionMode(cfg *config, newInstall bool) {
	if cfg.instant {
		getUI().setInstantMode(true)
		if err := writeProvisionMode(cfg.dir, provisionModeInstant); err != nil {
			fatal("Could not save the instant trial choice: %v", err)
		}
		return
	}
	// -fresh creates a new writable guest, so let the user choose again instead
	// of silently inheriting the previous guest's first-boot mode.
	if mode, ok := readProvisionMode(cfg.dir); ok && !cfg.fresh {
		cfg.instant = mode == provisionModeInstant
		getUI().setInstantMode(cfg.instant)
		return
	}
	if !newInstall {
		return
	}
	mode := provisionModePersonal
	if getUI().chooseInstantMode() {
		mode = provisionModeInstant
		cfg.instant = true
	}
	if setupCancelled() {
		return
	}
	getUI().setInstantMode(cfg.instant)
	if err := writeProvisionMode(cfg.dir, mode); err != nil {
		fatal("Could not save the first-boot choice: %v", err)
	}
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
	if err := ensureSettingsShortcutForExistingInstall(target, installDir); err != nil {
		logf("settings shortcut: %v", err)
	}
	if err := registerUninstallEntry(target, installDir); err != nil {
		logf("apps & features entry: %v", err)
	}
	if prefs, err := loadLaunchPreferences(installDir); err == nil {
		if err := syncSignInShortcut(target, installDir, prefs.LaunchAtSignIn); err != nil {
			logf("sign-in shortcut: %v", err)
		}
	} else {
		logf("launch preferences: %v", err)
	}
	if shortcutOfferRecorded(installDir) {
		return
	}
	startMenu, desktop := getUI().chooseShortcuts()
	if setupCancelled() {
		return
	}
	prefs, err := loadLaunchPreferences(installDir)
	if err != nil {
		logf("shortcut preferences: %v", err)
	}
	paths, err := launcherShortcutPaths()
	if err != nil {
		logf("shortcut paths: %v", err)
		errorBox("Try Omarchy is ready, but Windows could not find the shortcut locations.\n\n" + err.Error())
		return
	}
	message, err := finishLauncherShortcutChoice(paths, target, installDir, startMenu, desktop, prefs.StartAutomatically)
	if err != nil {
		logf("shortcuts: %v", err)
		errorBox("Try Omarchy is ready, but Windows could not finish creating shortcuts. Open TryOmarchy.exe in this installation's folder.\n\n" + err.Error())
	} else if message != "" {
		logf("shortcuts: using folder launchers")
		infoBox(message)
	}
}
