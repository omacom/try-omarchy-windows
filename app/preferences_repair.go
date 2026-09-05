package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Preserve the original preferences before replacing them with valid defaults.
// Only preferences are touched; guest disks, payloads, and shortcuts are separate.
func repairPreferences(path string) (string, error) {
	var write func() error
	switch filepath.Base(path) {
	case settingsFileName:
		write = func() error { return saveSettings(path, settings{ShareDisabled: true, SharedFolderPrompted: true}) }
	case storageSettingsFilename:
		write = func() error { return saveStorageSettings(filepath.Dir(path), 0) }
	default:
		return "", fmt.Errorf("not a supported preferences file")
	}
	return preserveAndRepairPreferences(path, write)
}

func preserveAndRepairPreferences(path string, write func() error) (string, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() || info.Size() > 4<<20 {
		return "", fmt.Errorf("preferences must be a regular file no larger than 4 MiB; the original was left unchanged")
	}
	source, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer source.Close()
	dir, err := os.MkdirTemp(filepath.Dir(path), "preferences-before-repair-*")
	if err != nil {
		return "", err
	}
	saved := filepath.Join(dir, filepath.Base(path))
	out, err := os.OpenFile(saved, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		os.RemoveAll(dir)
		return "", err
	}
	n, copyErr := io.Copy(out, io.LimitReader(source, (4<<20)+1))
	syncErr := out.Sync()
	closeErr := out.Close()
	if n != info.Size() {
		copyErr = fmt.Errorf("preferences changed while being copied")
	}
	if copyErr != nil || syncErr != nil || closeErr != nil {
		os.RemoveAll(dir)
		return "", fmt.Errorf("could not preserve preferences; the original was left unchanged: %v", errors.Join(copyErr, syncErr, closeErr))
	}
	if err := source.Close(); err != nil {
		return saved, err
	}
	current, err := os.Stat(path)
	if err != nil || !os.SameFile(info, current) || !current.ModTime().Equal(info.ModTime()) || current.Size() != info.Size() {
		return saved, fmt.Errorf("preferences changed during repair; try again")
	}
	if err = write(); err != nil {
		return saved, fmt.Errorf("could not save defaults; original preferences are backed up at %s: %w", saved, err)
	}
	return saved, nil
}
