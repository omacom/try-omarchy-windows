package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const (
	stableLauncherName  = "TryOmarchy.exe"
	shortcutOfferMarker = "shortcuts-offered-v1"
)

// copyLauncher stages a byte-for-byte copy beside the destination, flushes it,
// then lets the platform publish it in one replace operation. Copying the
// signed executable preserves its Authenticode signature.
func copyLauncher(source, target string, replace func(staged, target string) error) error {
	sourceInfo, err := os.Stat(source)
	if err != nil {
		return err
	}
	if targetInfo, statErr := os.Stat(target); statErr == nil && os.SameFile(sourceInfo, targetInfo) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}

	staged := target + ".part"
	if err := os.Remove(staged); err != nil && !os.IsNotExist(err) {
		return err
	}
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(staged, os.O_CREATE|os.O_EXCL|os.O_WRONLY, sourceInfo.Mode().Perm())
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			out.Close()
			os.Remove(staged)
		}
	}()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	if err := out.Sync(); err != nil {
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := replace(staged, target); err != nil {
		return fmt.Errorf("publishing stable launcher: %w", err)
	}
	ok = true
	return nil
}

func shortcutOfferRecorded(dir string) bool {
	info, err := os.Stat(filepath.Join(dir, shortcutOfferMarker))
	return err == nil && info.Mode().IsRegular()
}

func recordShortcutOffer(dir string) error {
	path := filepath.Join(dir, shortcutOfferMarker)
	staged := path + ".part"
	if err := os.WriteFile(staged, []byte("Shortcuts were offered after setup.\n"), 0o644); err != nil {
		return err
	}
	if err := os.Rename(staged, path); err != nil {
		os.Remove(staged)
		return err
	}
	return nil
}
