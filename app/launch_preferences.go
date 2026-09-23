package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Keep shortcut behavior separate from files understood by older launchers.
// A rollback can ignore this file and keep opening the pre-boot launcher.
const launchPreferencesFilename = "launch-preferences.json"

type launchPreferences struct {
	SchemaVersion      int  `json:"schemaVersion"`
	StartAutomatically bool `json:"startAutomatically"`
	LaunchAtSignIn     bool `json:"launchAtSignIn"`
}

func loadLaunchPreferences(dir string) (launchPreferences, error) {
	defaults := launchPreferences{SchemaVersion: 1}
	data, err := os.ReadFile(filepath.Join(dir, launchPreferencesFilename))
	if os.IsNotExist(err) {
		return defaults, nil
	}
	if err != nil {
		return defaults, err
	}
	if len(data) > 4096 {
		return defaults, fmt.Errorf("launch preferences are too large")
	}
	var p launchPreferences
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err = d.Decode(&p); err != nil {
		return defaults, err
	}
	if d.Decode(&struct{}{}) != io.EOF || p.SchemaVersion != 1 {
		return defaults, fmt.Errorf("invalid launch preferences")
	}
	return p, nil
}

func saveLaunchPreferences(dir string, p launchPreferences) error {
	p.SchemaVersion = 1
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".launch-preferences-*")
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
	return os.Rename(f.Name(), filepath.Join(dir, launchPreferencesFilename))
}
