//go:build !windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
)

func rejectMoveLink(path string, info os.FileInfo) error {
	if info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("linked paths cannot be moved: %s", path)
	}
	return nil
}
func rejectAncestorLink(path string, info os.FileInfo) error {
	return rejectMoveLink(path, info)
}
func publishMoveFile(from, to string) error {
	if err := os.Rename(from, to); err != nil {
		return err
	}
	f, err := os.Open(filepath.Dir(to))
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Sync()
}
func publishMoveDirectory(from, to string) error { return publishMoveFile(from, to) }
func lockMoveStore(s moveStore) (*os.File, error) {
	if err := os.MkdirAll(s.dir, 0700); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(filepath.Join(s.dir, "mutation.lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		return nil, err
	}
	return f, nil
}

func openMoveCleanupDisk(path string) (*os.File, error) { return openBackupDisk(path) }
