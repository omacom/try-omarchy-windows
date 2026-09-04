package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const storageSettingsFilename = "storage.json"
const maximumDiskGiB = 1024

// Storage preferences live separately so rollback to an older launcher, which
// rejects unknown settings.json fields, remains possible after choosing a size.
type storageSettings struct {
	SchemaVersion int `json:"schemaVersion"`
	DiskGiB       int `json:"diskGiB"`
}

func validateDiskGiB(size int) error {
	if size != 0 && (size < 24 || size > maximumDiskGiB) {
		return fmt.Errorf("disk capacity must be 0 (default) or between 24 and %d GiB", maximumDiskGiB)
	}
	return nil
}

func parseDiskGiB(value string) (int, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return 0, nil
	}
	size, err := strconv.Atoi(value)
	if err != nil {
		return 0, fmt.Errorf("disk capacity must be a whole number of GiB")
	}
	return size, validateDiskGiB(size)
}

func loadStorageSettings(dir string) (storageSettings, error) {
	defaults := storageSettings{SchemaVersion: 1}
	f, err := os.Open(filepath.Join(dir, storageSettingsFilename))
	if os.IsNotExist(err) {
		return defaults, nil
	}
	if err != nil {
		return defaults, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 4097))
	if err != nil {
		return defaults, err
	}
	if len(data) > 4096 {
		return defaults, fmt.Errorf("storage preferences are too large")
	}
	var settings storageSettings
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&settings); err != nil {
		return defaults, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return defaults, fmt.Errorf("storage preferences contain trailing data")
	}
	if settings.SchemaVersion != 1 {
		return defaults, fmt.Errorf("unsupported storage preferences version")
	}
	return settings, validateDiskGiB(settings.DiskGiB)
}

func saveStorageSettings(dir string, size int) error {
	if err := validateDiskGiB(size); err != nil {
		return err
	}
	data, err := json.Marshal(storageSettings{SchemaVersion: 1, DiskGiB: size})
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".storage-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err := f.Write(append(data, '\n')); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), filepath.Join(dir, storageSettingsFilename))
}

func requestedDiskMiB(factoryMiB int64, requestedGiB int, portable bool) (int64, error) {
	if factoryMiB <= 0 || factoryMiB > (1<<63-1)/(1024*1024) {
		return 0, fmt.Errorf("invalid expanded disk size: %d MiB", factoryMiB)
	}
	if err := validateDiskGiB(requestedGiB); err != nil {
		return 0, err
	}
	if portable && requestedGiB != 0 {
		return 0, fmt.Errorf("custom disk capacity is not supported in portable mode; use 0 to keep the existing portable disk")
	}
	requestedMiB := int64(requestedGiB) * 1024
	if requestedMiB > factoryMiB {
		return requestedMiB, nil
	}
	return factoryMiB, nil
}
