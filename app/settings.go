package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// settings is the launcher's persistent configuration, settings.json in the
// data directory. It carries the same rows the mac app's start menu has, so
// the settings window (when it lands) only edits this file, and every row
// stays usable today by editing the file or passing the matching flag. A
// flag given on the command line wins over the file for that row, so
// scripted launches stay predictable.
type settings struct {
	SchemaVersion int `json:"schemaVersion"`
	// Immersive: open fullscreen instead of in a window.
	Fullscreen bool `json:"fullscreen"`
	// Guest RAM in MiB. 0 sizes it to the machine automatically.
	MemoryMiB int `json:"memoryMiB"`
	// Windows folder shared into Omarchy. Empty means no share.
	Share string `json:"share"`
	// Loopback port forwards in -forward syntax, for example "tcp:2222:22".
	Forwards []string `json:"forwards"`
	// Public key file authorized for the Omarchy account when a forward
	// targets sshd. Empty picks the usual ~/.ssh/id_*.pub.
	SSHKey string `json:"sshKey"`
}

const (
	settingsSchemaVersion = 1
	settingsFileName      = "settings.json"
	minimumGuestMemoryMiB = 1024
	maximumGuestMemoryMiB = 262144
)

func settingsPath(dir string) string {
	return filepath.Join(dir, settingsFileName)
}

// loadSettings reads the file if it exists. A missing file is the default
// configuration, not an error; a damaged one is an error the user must see,
// because silently ignoring it would launch with the wrong memory or share.
func loadSettings(path string) (settings, error) {
	var s settings
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil {
		return s, err
	}
	if err := json.Unmarshal(data, &s); err != nil {
		return s, fmt.Errorf("%s: %v", path, err)
	}
	if s.SchemaVersion != settingsSchemaVersion {
		return s, fmt.Errorf("%s: schemaVersion %d is not supported by this launcher", path, s.SchemaVersion)
	}
	if err := s.validate(); err != nil {
		return s, fmt.Errorf("%s: %v", path, err)
	}
	return s, nil
}

// saveSettings writes atomically so an interrupted save cannot leave a
// half-written file that refuses the next launch.
func saveSettings(path string, s settings) error {
	s.SchemaVersion = settingsSchemaVersion
	if err := s.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
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

func (s settings) validate() error {
	if s.MemoryMiB != 0 && (s.MemoryMiB < minimumGuestMemoryMiB || s.MemoryMiB > maximumGuestMemoryMiB) {
		return fmt.Errorf("memoryMiB must be 0 (automatic) or between %d and %d", minimumGuestMemoryMiB, maximumGuestMemoryMiB)
	}
	var l forwardList
	for _, f := range s.Forwards {
		if err := l.Set(f); err != nil {
			return err
		}
	}
	return nil
}

// applySettings folds the file into the parsed flags. explicit holds the
// flag names the user actually passed; those rows keep the flag's value.
// Forwards are all-or-nothing: any -forward or -ssh on the command line
// replaces the file's list rather than merging with it.
func applySettings(cfg *config, s settings, explicit map[string]bool, forwards *forwardList, sshKeyPath *string) error {
	if !explicit["fullscreen"] {
		cfg.fullscreen = s.Fullscreen
	}
	if !explicit["memory"] {
		cfg.memOverrideMiB = s.MemoryMiB
	}
	if !explicit["share"] {
		cfg.share = s.Share
	}
	if !explicit["forward"] && !explicit["ssh"] {
		*forwards = nil
		for _, f := range s.Forwards {
			if err := forwards.Set(f); err != nil {
				return err
			}
		}
	}
	if !explicit["ssh-key"] {
		*sshKeyPath = s.SSHKey
	}
	return nil
}
