package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync/atomic"
	"time"
)

// Keep keyboard behavior separate from files that older launchers read
// strictly. A rollback ignores this file and keeps forwarding Alt+Tab.
const keyboardPreferencesFilename = "keyboard-preferences.json"

type keyboardPreferences struct {
	SchemaVersion   int  `json:"schemaVersion"`
	AltTabToWindows bool `json:"altTabToWindows"`
}

// altTabToWindows leaves Alt+Tab to the Windows task switcher instead of
// forwarding it to the guest while Omarchy is focused.
var altTabToWindows atomic.Bool

func loadKeyboardPreferences(dir string) (keyboardPreferences, error) {
	defaults := keyboardPreferences{SchemaVersion: 1}
	data, err := os.ReadFile(filepath.Join(dir, keyboardPreferencesFilename))
	if os.IsNotExist(err) {
		return defaults, nil
	}
	if err != nil {
		return defaults, err
	}
	if len(data) > 4096 {
		return defaults, fmt.Errorf("keyboard preferences are too large")
	}
	var p keyboardPreferences
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err = d.Decode(&p); err != nil {
		return defaults, err
	}
	if d.Decode(&struct{}{}) != io.EOF || p.SchemaVersion != 1 {
		return defaults, fmt.Errorf("invalid keyboard preferences")
	}
	return p, nil
}

func saveKeyboardPreferences(dir string, p keyboardPreferences) error {
	p.SchemaVersion = 1
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".keyboard-preferences-*")
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
	return os.Rename(f.Name(), filepath.Join(dir, keyboardPreferencesFilename))
}

// applyKeyboardPreferences publishes the saved choice to the keyboard hook.
// An unreadable file keeps the current behavior; lastErr suppresses repeats.
func applyKeyboardPreferences(dir string, lastErr *string) {
	p, err := loadKeyboardPreferences(dir)
	if err != nil {
		if err.Error() != *lastErr {
			logf("keyboard: preferences not readable, keeping the current Alt+Tab behavior: %v", err)
		}
		*lastErr = err.Error()
		return
	}
	*lastErr = ""
	if altTabToWindows.Swap(p.AltTabToWindows) != p.AltTabToWindows {
		if p.AltTabToWindows {
			logf("keyboard: Alt+Tab goes to Windows")
		} else {
			logf("keyboard: Alt+Tab goes to Omarchy while it is focused")
		}
	}
}

// watchKeyboardPreferences applies a choice saved in Settings, which runs in
// its own process, while Omarchy keeps running.
func watchKeyboardPreferences(dir string) {
	var lastErr string
	for {
		applyKeyboardPreferences(dir, &lastErr)
		time.Sleep(2 * time.Second)
	}
}
