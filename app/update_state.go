package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

const (
	updateStateVersion  = 1
	updateStateFilename = "update-state.json"
	maxUpdateStateBytes = 64 << 10
	updateCheckFilename = "last-update-check"
	updateCheckInterval = 4 * time.Hour
)

type launcherUpdateState struct {
	Schema      int    `json:"schema"`
	Version     string `json:"version"`
	SHA256      string `json:"sha256"`
	Started     bool   `json:"started"`
	HasPrevious bool   `json:"hasPrevious"`
}

func readLauncherUpdateState(dir string) (*launcherUpdateState, error) {
	data, err := os.ReadFile(filepath.Join(dir, updateStateFilename))
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(data) > maxUpdateStateBytes {
		return nil, fmt.Errorf("update state is too large")
	}
	var state launcherUpdateState
	if err := json.Unmarshal(data, &state); err != nil {
		return nil, err
	}
	if state.Schema != updateStateVersion || !validSHA256(state.SHA256) {
		return nil, fmt.Errorf("update state is invalid")
	}
	if _, ok := parsePreviewVersion(state.Version); !ok {
		return nil, fmt.Errorf("update state version is invalid")
	}
	return &state, nil
}

func writeLauncherUpdateState(dir string, state *launcherUpdateState) error {
	if state == nil || state.Schema != updateStateVersion || !validSHA256(state.SHA256) {
		return fmt.Errorf("refusing to write invalid update state")
	}
	data, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := filepath.Join(dir, updateStateFilename)
	staged := path + ".part"
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	f, err := os.OpenFile(staged, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			f.Close()
			os.Remove(staged)
		}
	}()
	if _, err := f.Write(data); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Rename(staged, path); err != nil {
		return err
	}
	ok = true
	return nil
}

func launcherUpdateDir(dir string) string {
	return filepath.Join(dir, "updates")
}

func previousLauncherPath(dir string) string {
	return filepath.Join(launcherUpdateDir(dir), "TryOmarchy.previous.exe")
}

func stagedLauncherPath(dir, version string) string {
	return filepath.Join(launcherUpdateDir(dir), version, stableLauncherName)
}

func clearLauncherUpdateState(dir string) error {
	if err := clearLauncherUpdateMarker(dir); err != nil {
		return err
	}
	if err := os.Remove(previousLauncherPath(dir)); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func clearLauncherUpdateMarker(dir string) error {
	for _, path := range []string{filepath.Join(dir, updateStateFilename), filepath.Join(dir, updateStateFilename) + ".part"} {
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

func updateCheckDue(dir string, now time.Time) bool {
	info, err := os.Stat(filepath.Join(dir, updateCheckFilename))
	if err != nil {
		return true
	}
	age := now.Sub(info.ModTime())
	return age < 0 || age >= updateCheckInterval
}

func recordUpdateCheck(dir string, now time.Time) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	path := filepath.Join(dir, updateCheckFilename)
	if err := os.WriteFile(path, []byte(now.UTC().Format(time.RFC3339)+"\n"), 0o644); err != nil {
		return err
	}
	return os.Chtimes(path, now, now)
}

func encodeRestartArgs(args []string) (string, error) {
	data, err := json.Marshal(args)
	if err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(data), nil
}

func decodeRestartArgs(encoded string) ([]string, error) {
	data, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil {
		return nil, err
	}
	var args []string
	if err := json.Unmarshal(data, &args); err != nil {
		return nil, err
	}
	if len(args) > 64 {
		return nil, fmt.Errorf("too many restart arguments")
	}
	for _, arg := range args {
		if len(arg) > 32768 {
			return nil, fmt.Errorf("restart argument is too long")
		}
	}
	return args, nil
}
