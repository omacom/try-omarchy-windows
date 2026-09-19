package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Keep new preferences outside settings.json: older launchers reject unknown
// fields there during update rollback. Missing preferences preserve v20 behavior.
const desktopPreferencesFilename = "desktop-preferences.json"

type desktopPreferences struct {
	SchemaVersion            int    `json:"schemaVersion"`
	CameraDisabled           bool   `json:"cameraDisabled"`
	CameraID                 string `json:"cameraID,omitempty"`
	MicrophoneDisabled       bool   `json:"microphoneDisabled"`
	AutomaticUpdatesDisabled bool   `json:"automaticUpdatesDisabled"`
}

func loadDesktopPreferences(dir string) (desktopPreferences, error) {
	defaults := desktopPreferences{SchemaVersion: 1}
	f, err := os.Open(filepath.Join(dir, desktopPreferencesFilename))
	if os.IsNotExist(err) {
		return defaults, nil
	}
	if err != nil {
		return defaults, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 16385))
	if err != nil {
		return defaults, err
	}
	if len(data) > 16384 {
		return defaults, fmt.Errorf("desktop preferences are too large")
	}
	var p desktopPreferences
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err = d.Decode(&p); err != nil {
		return defaults, err
	}
	if d.Decode(&struct{}{}) != io.EOF {
		return defaults, fmt.Errorf("desktop preferences contain trailing data")
	}
	if p.SchemaVersion != 1 || len(p.CameraID) > 4096 || strings.ContainsRune(p.CameraID, 0) {
		return defaults, fmt.Errorf("invalid desktop preferences")
	}
	return p, nil
}
func saveDesktopPreferences(dir string, p desktopPreferences) error {
	p.SchemaVersion = 1
	if len(p.CameraID) > 4096 || strings.ContainsRune(p.CameraID, 0) {
		return fmt.Errorf("invalid camera selection")
	}
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".desktop-preferences-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(append(data, '\n')); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), filepath.Join(dir, desktopPreferencesFilename))
}
func memoryGiBText(mib int) string { return strconv.FormatFloat(float64(mib)/1024, 'f', -1, 64) }
func memoryMiBFromGiB(value string) (string, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return "0", nil
	}
	n, err := strconv.ParseFloat(value, 64)
	if err != nil || !(n >= 0 && n <= 64) {
		return "", fmt.Errorf("memory must be between 1 and 64 GB, or 0 for automatic")
	}
	mib := int(n * 1024)
	if float64(mib) != n*1024 || (mib != 0 && mib < minimumGuestMemoryMiB) {
		return "", fmt.Errorf("memory must be between 1 and 64 GB, or 0 for automatic")
	}
	return strconv.Itoa(mib), nil
}
