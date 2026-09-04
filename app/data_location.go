package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const (
	dataLocationPointerVersion = 1
	dataLocationPointerName    = "data-location.json"
	maxDataLocationBytes       = 16 << 10
)

// Kept as a variable so isolated signed test builds can use their own data
// directory without changing production behavior.
var defaultDataDirectoryName = "TryOmarchy"

type dataLocationPointer struct {
	Version int    `json:"version"`
	Path    string `json:"path"`
}

type dataLocationChooser func(defaultDir string) (selected string, proceed bool, err error)

// resolveStandardDataDirectory keeps the startup precedence in one tested
// place: an explicit -dir wins, then a remembered alternate location, then an
// existing default install, and finally the first-run chooser.
func resolveStandardDataDirectory(defaultDir, requested string, explicit, prompt bool, choose dataLocationChooser) (string, bool, error) {
	if explicit {
		return requested, true, nil
	}
	selected, found, err := loadDataLocationPointer(defaultDir)
	if err != nil {
		return "", false, err
	}
	if found {
		return selected, true, nil
	}
	if !prompt {
		return defaultDir, true, nil
	}
	unclaimed, err := dataDirectoryUnclaimed(defaultDir)
	if err != nil {
		return "", false, err
	}
	if !unclaimed {
		return defaultDir, true, nil
	}
	if choose == nil {
		return "", false, fmt.Errorf("data location chooser is unavailable")
	}
	selected, proceed, err := choose(defaultDir)
	if err != nil || !proceed {
		return selected, proceed, err
	}
	selected, err = validateDataLocationPath(selected)
	if err != nil {
		return "", false, err
	}
	if !pathsEqual(selected, defaultDir) {
		if err := saveDataLocationPointer(defaultDir, selected); err != nil {
			return "", false, err
		}
	}
	return selected, true, nil
}

func dataLocationPointerPath(defaultDir string) string {
	return filepath.Join(defaultDir, dataLocationPointerName)
}

// loadDataLocationPointer resolves the small bootstrap record kept in the
// default Local AppData directory. The record is needed because the launcher
// cannot discover an installation on another drive before it knows where to
// look. Explicit -dir and portable launches bypass it in main.
func loadDataLocationPointer(defaultDir string) (string, bool, error) {
	path := dataLocationPointerPath(defaultDir)
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return "", false, nil
	}
	if err != nil {
		return "", false, err
	}
	if !info.Mode().IsRegular() {
		return "", false, fmt.Errorf("%s is not a regular file", path)
	}
	if info.Size() > maxDataLocationBytes {
		return "", false, fmt.Errorf("%s is too large", path)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", false, err
	}
	dec := json.NewDecoder(strings.NewReader(string(data)))
	dec.DisallowUnknownFields()
	var pointer dataLocationPointer
	if err := dec.Decode(&pointer); err != nil {
		return "", false, fmt.Errorf("%s: %v", path, err)
	}
	if err := dec.Decode(&struct{}{}); err != io.EOF {
		return "", false, fmt.Errorf("%s: data contains trailing content", path)
	}
	if pointer.Version != dataLocationPointerVersion {
		return "", false, fmt.Errorf("%s: version %d is not supported", path, pointer.Version)
	}
	selected, err := validateDataLocationPath(pointer.Path)
	if err != nil {
		return "", false, fmt.Errorf("%s: %v", path, err)
	}
	defaultPath, err := validateDataLocationPath(defaultDir)
	if err != nil {
		return "", false, err
	}
	if pathsEqual(selected, defaultPath) {
		return "", false, fmt.Errorf("%s points back to the default data directory", path)
	}
	return selected, true, nil
}

func saveDataLocationPointer(defaultDir, selected string) error {
	defaultPath, err := validateDataLocationPath(defaultDir)
	if err != nil {
		return err
	}
	selectedPath, err := validateDataLocationPath(selected)
	if err != nil {
		return err
	}
	if pathsEqual(defaultPath, selectedPath) {
		return fmt.Errorf("alternate data directory matches the default data directory")
	}
	if err := os.MkdirAll(defaultPath, 0o755); err != nil {
		return err
	}
	pointer := dataLocationPointer{Version: dataLocationPointerVersion, Path: selectedPath}
	data, err := json.MarshalIndent(pointer, "", "  ")
	if err != nil {
		return err
	}
	path := dataLocationPointerPath(defaultPath)
	tmp := path + ".part"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func validateDataLocationPath(path string) (string, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return "", fmt.Errorf("data directory is empty")
	}
	if !filepath.IsAbs(path) {
		return "", fmt.Errorf("data directory must be an absolute path")
	}
	return filepath.Clean(path), nil
}

func dataDirectoryForSelection(parent string) (string, error) {
	parent, err := validateDataLocationPath(parent)
	if err != nil {
		return "", err
	}
	if strings.EqualFold(filepath.Base(parent), defaultDataDirectoryName) {
		return parent, nil
	}
	return filepath.Join(parent, defaultDataDirectoryName), nil
}

func dataDirectoryUnclaimed(path string) (bool, error) {
	entries, err := os.ReadDir(path)
	if errors.Is(err, os.ErrNotExist) {
		return true, nil
	}
	if err != nil {
		return false, err
	}
	return len(entries) == 0, nil
}

func ensureDataDirectoryWritable(path string) error {
	if err := os.MkdirAll(path, 0o755); err != nil {
		return err
	}
	f, err := os.CreateTemp(path, ".tryomarchy-write-test-*")
	if err != nil {
		return err
	}
	name := f.Name()
	if err := f.Close(); err != nil {
		os.Remove(name)
		return err
	}
	if err := os.Remove(name); err != nil {
		return err
	}
	return nil
}
