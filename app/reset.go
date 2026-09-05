package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Prepare a replacement before moving the old disk. The retained folder also
// survives a process interruption between the two publication renames.
func resetStandardDisk(cfg *config, expandedMiB int64) (string, error) {
	if cfg.portable {
		return "", fmt.Errorf("reset with recovery is only supported for standard installs")
	}
	for _, name := range []string{payloadUpdateStateFilename, updateStateFilename} {
		if _, err := os.Lstat(filepath.Join(cfg.dir, name)); !os.IsNotExist(err) {
			return "", fmt.Errorf("finish the pending update before resetting")
		}
	}
	if info, err := os.Lstat(cfg.disk); os.IsNotExist(err) {
		copy := *cfg
		copy.fresh = false
		return "", prepareDisk(&copy, expandedMiB)
	} else if err != nil {
		return "", err
	} else if !info.Mode().IsRegular() {
		return "", fmt.Errorf("reset requires a regular disk file")
	}
	lock, err := openBackupDisk(cfg.disk)
	if err != nil {
		return "", fmt.Errorf("close Omarchy before resetting: %w", err)
	}
	defer lock.Close()
	stage, err := os.MkdirTemp(cfg.vmDir, ".reset-staging-*")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(stage)
	next := *cfg
	next.fresh = false
	next.disk = filepath.Join(stage, "disk.raw")
	next.vmDir = stage
	if err = prepareDisk(&next, expandedMiB); err != nil {
		return "", err
	}
	if err = checkSetupCancelled(); err != nil {
		return "", err
	}
	retained, err := os.MkdirTemp(cfg.vmDir, "before-reset-*")
	if err != nil {
		return "", err
	}
	old := filepath.Join(retained, "disk.raw")
	if err = lock.Close(); err != nil {
		os.Remove(retained)
		return "", err
	}
	if err = publishResetDisk(cfg.disk, next.disk, old, os.Rename); err != nil {
		// Remove only an empty retention folder. A failed rollback leaves the
		// previous disk there and reports its location to the user.
		_ = os.Remove(retained)
		return "", err
	}
	return old, nil
}

func publishResetDisk(current, staged, retained string, rename func(string, string) error) error {
	if err := rename(current, retained); err != nil {
		return err
	}
	if err := rename(staged, current); err != nil {
		if rollbackErr := rename(retained, current); rollbackErr != nil {
			return fmt.Errorf("reset failed: %v; previous disk remains at %s: %w", err, retained, rollbackErr)
		}
		return err
	}
	return nil
}
