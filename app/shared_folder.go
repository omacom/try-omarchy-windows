package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const recommendedSharedFolderName = "Omarchy Shared"

func recommendedSharedFolderPath(home string) (string, error) {
	home = strings.TrimSpace(home)
	if home == "" || !filepath.IsAbs(home) {
		return "", fmt.Errorf("Windows did not provide a usable home folder")
	}
	return filepath.Join(filepath.Clean(home), recommendedSharedFolderName), nil
}

func createRecommendedSharedFolder(home string) (string, error) {
	path, err := recommendedSharedFolderPath(home)
	if err != nil {
		return "", err
	}
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		if err := os.Mkdir(path, 0o755); err != nil {
			return "", fmt.Errorf("creating %s: %w", path, err)
		}
		return path, nil
	}
	if err != nil {
		return "", fmt.Errorf("checking %s: %w", path, err)
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
		return "", fmt.Errorf("%s already exists and is not a regular folder", path)
	}
	return path, nil
}
