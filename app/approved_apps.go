package main

import (
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const approvedAppsFilename = "approved-windows-apps.json"
const maximumApprovedApps = 16
const maximumApprovedAppsBytes = 128 << 10

type approvedWindowsApp struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	Path string `json:"path"`
}

type approvedAppPreferences struct {
	SchemaVersion int                  `json:"schemaVersion"`
	Apps          []approvedWindowsApp `json:"apps"`
}

func approvedAppsPath(dir string) string { return filepath.Join(dir, approvedAppsFilename) }

func validApprovedAppID(id string) bool {
	if len(id) != 32 {
		return false
	}
	for _, c := range id {
		if !strings.ContainsRune("0123456789abcdef", c) {
			return false
		}
	}
	return true
}

func (prefs approvedAppPreferences) validate() error {
	if prefs.SchemaVersion != 1 || len(prefs.Apps) > maximumApprovedApps {
		return fmt.Errorf("invalid approved Windows apps file")
	}
	seen := make(map[string]bool, len(prefs.Apps))
	for _, app := range prefs.Apps {
		if !validApprovedAppID(app.ID) || seen[app.ID] || app.Name == "" || app.Name != strings.TrimSpace(app.Name) || len([]rune(app.Name)) > 80 || app.Path == "" || len(app.Path) > 4096 {
			return fmt.Errorf("invalid approved Windows app")
		}
		for _, value := range []string{app.Name, app.Path} {
			for _, ch := range value {
				if ch < 0x20 || ch == 0x7f {
					return fmt.Errorf("control character in approved Windows app")
				}
			}
		}
		seen[app.ID] = true
	}
	return nil
}

func loadApprovedWindowsApps(dir string) (approvedAppPreferences, error) {
	prefs := approvedAppPreferences{SchemaVersion: 1, Apps: []approvedWindowsApp{}}
	path := approvedAppsPath(dir)
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return prefs, nil
	}
	if err != nil {
		return prefs, err
	}
	if !info.Mode().IsRegular() || info.Size() > maximumApprovedAppsBytes {
		return prefs, fmt.Errorf("approved Windows apps file is not a regular file under 128 KiB")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return prefs, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&prefs); err != nil {
		return prefs, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return prefs, fmt.Errorf("approved Windows apps file has trailing data")
	}
	return prefs, prefs.validate()
}

func saveApprovedWindowsApps(dir string, prefs approvedAppPreferences) error {
	prefs.SchemaVersion = 1
	if err := prefs.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(prefs, "", "  ")
	if err != nil {
		return err
	}
	if len(data) > maximumApprovedAppsBytes {
		return fmt.Errorf("approved Windows apps file is too large")
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	path := approvedAppsPath(dir)
	tmp := path + ".part"
	if err := os.WriteFile(tmp, append(data, '\n'), 0600); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func newApprovedAppID() (string, error) {
	var data [16]byte
	if _, err := rand.Read(data[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(data[:]), nil
}

func approvedAppsLine(prefs approvedAppPreferences) (string, error) {
	type entry struct {
		ID   string `json:"id"`
		Name string `json:"name"`
	}
	entries := make([]entry, 0, len(prefs.Apps))
	for _, app := range prefs.Apps {
		entries = append(entries, entry{app.ID, app.Name})
	}
	data, err := json.Marshal(entries)
	if err != nil {
		return "", err
	}
	return "apps " + base64.StdEncoding.EncodeToString(data) + "\n", nil
}
