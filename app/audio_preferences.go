package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf8"
)

// Separate from desktop-preferences.json so older launchers can still roll back.
const audioPreferencesFilename = "audio-preferences.json"

type audioPreferences struct {
	SchemaVersion int    `json:"schemaVersion"`
	Output        string `json:"output,omitempty"`
	Input         string `json:"input,omitempty"`
}

func (p audioPreferences) validate() error {
	if p.SchemaVersion != 1 {
		return fmt.Errorf("unsupported audio preferences version")
	}
	for _, name := range []string{p.Output, p.Input} {
		if len(name) > 4096 || strings.ContainsRune(name, 0) || !utf8.ValidString(name) {
			return fmt.Errorf("invalid audio device name")
		}
	}
	return nil
}

func loadAudioPreferences(dir string) (audioPreferences, error) {
	p := audioPreferences{SchemaVersion: 1}
	f, err := os.Open(filepath.Join(dir, audioPreferencesFilename))
	if os.IsNotExist(err) {
		return p, nil
	}
	if err != nil {
		return p, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 16385))
	if err != nil {
		return p, err
	}
	if len(data) > 16384 {
		return p, fmt.Errorf("audio preferences are too large")
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	p = audioPreferences{}
	if err := d.Decode(&p); err != nil {
		return p, err
	}
	if d.Decode(&struct{}{}) != io.EOF {
		return p, fmt.Errorf("audio preferences contain trailing data")
	}
	return p, p.validate()
}

func saveAudioPreferences(dir string, p audioPreferences) error {
	p.SchemaVersion = 1
	if err := p.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if len(data)+1 > 16384 {
		return fmt.Errorf("audio preferences are too large")
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".audio-preferences-*")
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
	return os.Rename(f.Name(), filepath.Join(dir, audioPreferencesFilename))
}

// SDL's generic override applies the same name to both directions. Always
// remove it, along with inherited launcher overrides, before setting our own.
func audioEnvironment(env []string, p audioPreferences, enabled, microphoneDisabled bool) []string {
	result := make([]string, 0, len(env)+2)
	for _, item := range env {
		key, _, _ := strings.Cut(item, "=")
		switch strings.ToUpper(key) {
		case "SDL_AUDIO_DEVICE_NAME", "OMARCHY_SDL_OUTPUT_DEVICE_NAME", "OMARCHY_SDL_INPUT_DEVICE_NAME":
			continue
		}
		result = append(result, item)
	}
	if enabled {
		if p.Output != "" {
			result = append(result, "OMARCHY_SDL_OUTPUT_DEVICE_NAME="+p.Output)
		}
		if p.Input != "" && !microphoneDisabled {
			result = append(result, "OMARCHY_SDL_INPUT_DEVICE_NAME="+p.Input)
		}
	}
	return result
}

// The bundled provenance travels with the executable through update/rollback.
// Older and stock runtimes must not silently claim to apply these preferences.
func audioRuntimeSupportsSelection(qemu string) bool {
	return runtimeHasPatch(qemu, "patches/qemu/0013-select-sdl-audio-devices.patch")
}

func runtimeHasPatch(qemu, patch string) bool {
	path := filepath.Join(filepath.Dir(filepath.Dir(qemu)), "provenance", "sources.lock.json")
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	var lock struct {
		QEMU struct {
			Patches []struct {
				File string `json:"file"`
			} `json:"patches"`
		} `json:"qemu"`
	}
	if json.NewDecoder(io.LimitReader(f, 65536)).Decode(&lock) != nil {
		return false
	}
	for _, p := range lock.QEMU.Patches {
		if p.File == patch {
			return true
		}
	}
	return false
}
