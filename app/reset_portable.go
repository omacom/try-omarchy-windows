package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Portable disks and their backing identities travel together. Publish the
// identity before its new disk; an interruption must never pair an old disk
// with the replacement factory's identity.
func resetPortableDisk(cfg *config, expandedBytes int64) error {
	existing := make([]string, 0, 2)
	for _, path := range []string{cfg.disk, portableBackingStatePath(cfg.disk)} {
		info, err := os.Lstat(path)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("reset requires regular disk and identity files: %s", path)
		}
		existing = append(existing, path)
	}
	if len(existing) == 0 {
		return preparePortableDisk(cfg, expandedBytes)
	}
	var lock *os.File
	if existing[0] == cfg.disk {
		var err error
		lock, err = openBackupDisk(cfg.disk)
		if err != nil {
			return fmt.Errorf("close Omarchy before resetting: %w", err)
		}
		defer lock.Close()
	}
	stage, err := os.MkdirTemp(cfg.vmDir, ".reset-staging-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	next := *cfg
	next.fresh = false
	next.vmDir = stage
	next.disk = filepath.Join(stage, filepath.Base(cfg.disk))
	if err := preparePortableDisk(&next, expandedBytes); err != nil {
		return err
	}
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	retained, err := os.MkdirTemp(cfg.vmDir, "before-reset-*")
	if err != nil {
		return err
	}
	if lock != nil {
		if err := lock.Close(); err != nil {
			os.Remove(retained)
			return err
		}
	}
	moves := make([]resetMove, 0, 4)
	for _, path := range existing {
		moves = append(moves, resetMove{path, filepath.Join(retained, filepath.Base(path))})
	}
	moves = append(moves,
		resetMove{portableBackingStatePath(next.disk), portableBackingStatePath(cfg.disk)},
		resetMove{next.disk, cfg.disk},
	)
	if err := publishPortableReset(moves, os.Rename); err != nil {
		_ = os.Remove(retained) // Only remove an empty folder after successful rollback.
		return fmt.Errorf("portable reset failed; retained files, if any, are in %s: %w", retained, err)
	}
	logf("previous portable disk retained in %s", retained)
	return nil
}

type resetMove struct{ from, to string }

func publishPortableReset(moves []resetMove, rename func(string, string) error) error {
	for i, move := range moves {
		if err := rename(move.from, move.to); err != nil {
			for j := i - 1; j >= 0; j-- {
				undo := moves[j]
				if rollbackErr := rename(undo.to, undo.from); rollbackErr != nil {
					// Stop here. Restoring an older disk while the new identity remains
					// active would make a mismatched backing image appear trustworthy.
					return fmt.Errorf("%v; could not restore %s: %w", err, undo.to, rollbackErr)
				}
			}
			return err
		}
	}
	return nil
}
