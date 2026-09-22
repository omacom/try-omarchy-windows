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

// Endpoint IDs survive device renames and reboots where friendly names do not.
// They live in their own file so a rolled-back launcher that only knows
// audio-preferences.json keeps working: that decoder rejects unknown fields.
const audioEndpointsFilename = "audio-endpoints.json"

type audioEndpoints struct {
	SchemaVersion int    `json:"schemaVersion"`
	OutputID      string `json:"outputId,omitempty"`
	InputID       string `json:"inputId,omitempty"`
}

type audioEndpointInfo struct {
	ID   string
	Name string
}

type mmDeviceList struct {
	Output, Input []audioEndpointInfo
}

func (p audioEndpoints) validate() error {
	if p.SchemaVersion != 1 {
		return fmt.Errorf("unsupported audio endpoint version")
	}
	for _, id := range []string{p.OutputID, p.InputID} {
		if len(id) > 4096 || strings.ContainsRune(id, 0) || !utf8.ValidString(id) {
			return fmt.Errorf("invalid audio endpoint id")
		}
	}
	return nil
}

func loadAudioEndpoints(dir string) (audioEndpoints, error) {
	p := audioEndpoints{SchemaVersion: 1}
	f, err := os.Open(filepath.Join(dir, audioEndpointsFilename))
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
		return p, fmt.Errorf("audio endpoints are too large")
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	p = audioEndpoints{}
	if err = d.Decode(&p); err != nil {
		return p, err
	}
	if d.Decode(&struct{}{}) != io.EOF {
		return p, fmt.Errorf("audio endpoints contain trailing data")
	}
	return p, p.validate()
}

func saveAudioEndpoints(dir string, p audioEndpoints) error {
	p.SchemaVersion = 1
	if err := p.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if len(data)+1 > 16384 {
		return fmt.Errorf("audio endpoints are too large")
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".audio-endpoints-*")
	if err != nil {
		return err
	}
	temp := f.Name()
	defer os.Remove(temp)
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
	return os.Rename(temp, filepath.Join(dir, audioEndpointsFilename))
}

// saveAudioSelection clears the ID file before changing the friendly-name
// preferences. An interrupted two-file update can therefore lose the stable
// ID optimization, but it cannot pair a newly selected name with an old ID.
func saveAudioSelection(dir string, names audioPreferences, endpoints audioEndpoints) error {
	if err := saveAudioEndpoints(dir, audioEndpoints{}); err != nil {
		return err
	}
	if err := saveAudioPreferences(dir, names); err != nil {
		return err
	}
	return saveAudioEndpoints(dir, endpoints)
}

// resolveAudioSelection prefers the stable endpoint ID and falls back to the
// remembered friendly name when the endpoint is missing. The runtime still
// receives a friendly name because that is what SDL matches today.
func resolveAudioSelection(selectedName, selectedID string, live []audioEndpointInfo) (name string, id string) {
	if selectedID != "" {
		for _, endpoint := range live {
			if endpoint.ID == selectedID && endpoint.Name != "" {
				return endpoint.Name, endpoint.ID
			}
		}
	}
	if selectedName != "" {
		var matchedID string
		for _, endpoint := range live {
			if endpoint.Name == selectedName {
				if matchedID != "" && matchedID != endpoint.ID {
					return selectedName, selectedID
				}
				matchedID = endpoint.ID
			}
		}
		if matchedID != "" {
			return selectedName, matchedID
		}
	}
	return selectedName, selectedID
}

// endpointIDForSelection records an ID only when the selected friendly name
// identifies one live endpoint. Leaving the remembered selection unchanged
// preserves its ID while the device is disconnected.
func endpointIDForSelection(selectedName, rememberedName, rememberedID string, live []audioEndpointInfo) string {
	if selectedName == "" {
		return ""
	}
	if rememberedID != "" && selectedName == rememberedName {
		return rememberedID
	}
	var found string
	for _, endpoint := range live {
		if endpoint.Name != selectedName {
			continue
		}
		if found != "" && found != endpoint.ID {
			// Duplicate friendly names cannot be mapped safely without changing
			// the runtime interface, so retain name-based behavior for them.
			return ""
		}
		found = endpoint.ID
	}
	return found
}
