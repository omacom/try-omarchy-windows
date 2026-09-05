//go:build windows

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

func chooseBackupDestination() (string, bool, error) {
	for {
		name, ok, err := chooseRecoveryPath(0, "Save a new Omarchy backup", "Omarchy-"+time.Now().Format("2006-01-02-150405")+".zip", true, false)
		if err != nil || !ok {
			return "", ok, err
		}
		if _, err = os.Lstat(name); os.IsNotExist(err) {
			return name, true, nil
		} else if err != nil {
			return "", false, err
		}
		infoBox("A file already exists there. Choose a new filename to keep both backups.")
	}
}

func beginRecoveryProgress(status string) {
	ui := getUI()
	ui.cancelMessage.Store("Cancel this operation?\n\nThe original installation and completed backups will be kept. Temporary files will be removed.")
	ui.setProgress(0, 0)
	ui.setStatus("%s", status)
}

func runRecoveryUI(dir, action string) error {
	configureSetupCancellation(false)
	switch action {
	case "backup":
		name, ok, err := chooseBackupDestination()
		if err != nil || !ok {
			return err
		}
		beginRecoveryProgress("Creating your VM backup...")
		defer uiDone()
		if err = writeVMBackupProgress(dir, name, recoveryProgress("Backing up")); err != nil {
			return err
		}
		uiDone()
		infoBox("Backup saved to:\n\n" + name + "\n\nIt contains personal guest files and is not encrypted. Keep it private.")
	case "restore":
		source, ok, err := chooseRecoveryPath(0, "Choose a trusted Omarchy backup", "", false, false)
		if err != nil || !ok {
			return err
		}
		parent, ok, err := chooseRecoveryPath(0, "Choose where to create the restored copy", "", false, true)
		if err != nil || !ok {
			return err
		}
		destination := filepath.Join(parent, "OmarchyRestored-"+time.Now().Format("2006-01-02-150405"))
		if msgBox("Restore this backup into a new folder?\n\n"+destination+"\n\nYour current installation and shortcuts will stay unchanged. Only restore backups you trust.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
			return nil
		}
		beginRecoveryProgress("Verifying and restoring your VM...")
		defer uiDone()
		if err = restoreVMBackupProgress(source, destination, recoveryProgress("Restoring")); err != nil {
			return err
		}
		uiDone()
		if err := createRestoredLaunchers(destination); err != nil {
			infoBox("Your data was restored to:\n\n" + destination + "\n\nThe startup shortcuts could not be created: " + err.Error() + "\n\nYou can start this copy with -dir pointing to its folder.")
		} else {
			// The restored copy has its shortcuts; the first launch must not
			// offer them again.
			if err := recordShortcutOffer(destination); err != nil {
				logf("could not record the shortcut offer for %s: %v", destination, err)
			}
			infoBox("Restored to:\n\n" + destination + "\n\nOpen Start Omarchy in that folder to use this copy, or Settings to review it first. Your original installation and shortcuts are unchanged.")
		}
	case "reset":
		return resetFromSettings(dir)
	case "uninstall":
		return runUninstall(dir)
	default:
		return fmt.Errorf("unknown recovery action")
	}
	return nil
}

// Backup failure or cancellation must never fall through into reset.
func confirmResetBackup(dir string) (bool, error) {
	choice := msgBox("Start over with a clean Omarchy guest?\n\nThis resets the guest account, installed apps, and guest files. Windows shared folders and launcher settings are kept. The old disk will be retained for recovery.\n\nCreate a full backup first?\nYes: choose a backup. No: skip the full backup. Cancel: do nothing.", 3|mbIconQuestion|0x200)
	if choice != idYes && choice != idNo {
		return false, nil
	}
	if choice == idYes {
		name, ok, err := chooseBackupDestination()
		if err != nil || !ok {
			return false, err
		}
		beginRecoveryProgress("Backing up before reset...")
		if err = writeVMBackupProgress(dir, name, recoveryProgress("Backing up")); err != nil {
			return false, err
		}
	}
	if msgBox("Reset the active Omarchy guest now?\n\nThe old disk will remain in the VM folder until you remove it. The new guest needs first-run setup.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return false, nil
	}
	return !setupCancelled(), checkSetupCancelled()
}

func resetFromSettings(dir string) error {
	if !completeInstallExists(dir, "disk.raw") {
		return fmt.Errorf("there is no complete standard installation to reset")
	}
	proceed, err := confirmResetBackup(dir)
	if err != nil || !proceed {
		return err
	}
	data, err := os.ReadFile(filepath.Join(dir, "guest", "build-spec.json"))
	if err != nil {
		return err
	}
	var spec buildSpec
	if err = json.Unmarshal(data, &spec); err != nil {
		return err
	}
	storage, err := loadStorageSettings(dir)
	if err != nil {
		return err
	}
	cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest"), vmDir: filepath.Join(dir, "vm"), disk: filepath.Join(dir, "vm", "disk.raw"), diskFormat: "raw", diskGiB: storage.DiskGiB}
	beginRecoveryProgress("Preparing a clean Omarchy guest...")
	old, err := resetStandardDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB)
	if err != nil {
		return err
	}
	uiDone()
	infoBox("Omarchy is ready for a fresh start on its next launch.\n\nThe previous disk is kept at:\n" + old + "\n\nKeep it until you have checked the new guest. It continues to use Windows disk space.")
	return nil
}

func reportRecoveryResult(err error) {
	uiDone()
	if err != nil && !errors.Is(err, errSetupCancelled) {
		errorBox("Try Omarchy could not finish this operation.\n\n" + err.Error())
	}
}

// Place launchers beside the restored data, never over the original desktop or
// Start-menu links. The explicit -dir argument keeps both installations separate.
func createRestoredLaunchers(dir string) error {
	target, err := stableLauncherPath(dir)
	if err != nil {
		return err
	}
	const script = `$ErrorActionPreference='Stop'; $shell=New-Object -ComObject WScript.Shell; ` +
		`$items=@(@('Start Omarchy.lnk',$env:TRYOMARCHY_RESTORE_ARGS),@('Settings.lnk',($env:TRYOMARCHY_RESTORE_ARGS+' -settings'))); ` +
		`foreach($item in $items){$path=Join-Path $env:TRYOMARCHY_RESTORE_DIR $item[0]; if(Test-Path -LiteralPath $path){throw 'Shortcut already exists'}; ` +
		`$link=$shell.CreateShortcut($path); $link.TargetPath=$env:TRYOMARCHY_RESTORE_TARGET; $link.Arguments=$item[1]; $link.WorkingDirectory=$env:TRYOMARCHY_RESTORE_DIR; $link.IconLocation=$env:TRYOMARCHY_RESTORE_TARGET+',0'; $link.Save()}`
	cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", script)
	cmd.Env = append(os.Environ(), "TRYOMARCHY_RESTORE_DIR="+dir, "TRYOMARCHY_RESTORE_TARGET="+target, "TRYOMARCHY_RESTORE_ARGS="+shortcutArguments(dir))
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("creating restored shortcuts: %w: %s", err, strings.TrimSpace(string(output)))
	}
	return recordShortcutOffer(dir)
}

func recoveryProgress(action string) backupProgress {
	last := ""
	return func(current, total int64, name string) {
		ui := getUI()
		if name != last {
			ui.setStatus("%s %s...", action, name)
			last = name
		}
		ui.setProgress(current, total)
	}
}
