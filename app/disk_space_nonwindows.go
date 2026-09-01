//go:build !windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
)

func existingDiskPath(path string) (string, error) {
	path = filepath.Clean(path)
	for {
		if _, err := os.Stat(path); err == nil {
			return path, nil
		} else if !os.IsNotExist(err) {
			return "", err
		}
		parent := filepath.Dir(path)
		if parent == path {
			return "", fmt.Errorf("no existing parent for %s", path)
		}
		path = parent
	}
}

func platformDiskFreeBytes(path string) (int64, error) {
	existing, err := existingDiskPath(path)
	if err != nil {
		return 0, err
	}
	var stat syscall.Statfs_t
	if err := syscall.Statfs(existing, &stat); err != nil {
		return 0, err
	}
	available := uint64(stat.Bavail) * uint64(stat.Bsize)
	if available > uint64(1<<63-1) {
		return 1<<63 - 1, nil
	}
	return int64(available), nil
}

func platformAllocatedFileBytes(path string) (int64, error) {
	var stat syscall.Stat_t
	if err := syscall.Stat(path, &stat); err != nil {
		return 0, err
	}
	if stat.Blocks < 0 || stat.Blocks > (1<<63-1)/512 {
		return 0, fmt.Errorf("allocated file size is invalid")
	}
	return stat.Blocks * 512, nil
}
