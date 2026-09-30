package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// USB grants are separate so older launchers can safely ignore them on rollback.
const usbPreferencesFilename = "usb-preferences.json"

type usbSelection struct {
	Bus     int    `json:"bus"`
	Port    string `json:"port"`
	Vendor  int    `json:"vendor"`
	Product int    `json:"product"`
	Name    string `json:"name"`
}
type usbPreferences struct {
	SchemaVersion int           `json:"schemaVersion"`
	Enabled       bool          `json:"enabled"`
	Device        *usbSelection `json:"device,omitempty"`
}

func (p usbPreferences) validate() error {
	if p.Enabled && p.Device == nil {
		return fmt.Errorf("choose a USB device before enabling attachment")
	}
	if d := p.Device; d != nil {
		if err := (usbDevice{Bus: d.Bus, Address: 1, Port: d.Port, Vendor: d.Vendor, Product: d.Product}).validate(); err != nil {
			return err
		}
		if len(d.Name) > 512 || strings.ContainsAny(d.Name, "\x00\r\n") {
			return fmt.Errorf("invalid USB device name")
		}
	}
	return nil
}
func selectionForUSB(d usbDevice) *usbSelection {
	return &usbSelection{d.Bus, d.Port, d.Vendor, d.Product, d.Name}
}
func (s usbSelection) matches(d usbDevice) bool {
	return s.Bus == d.Bus && s.Port == d.Port && s.Vendor == d.Vendor && s.Product == d.Product
}
func usbSelectionChoices(devices []usbDevice, saved *usbSelection) []usbDevice {
	if saved == nil {
		return devices
	}
	for _, d := range devices {
		if saved.matches(d) {
			return devices
		}
	}
	d := usbDevice{Bus: saved.Bus, Address: 1, Port: saved.Port, Vendor: saved.Vendor, Product: saved.Product, Name: saved.Name, Connected: false}
	d.ID = d.identity()
	return append(devices, d)
}

// A missing or busy choice never prevents VM boot. Resolve a fresh enumeration
// address only after matching the saved physical port and both device IDs.
func (b usbBroker) AttachSaved(ctx context.Context, p usbPreferences) error {
	if err := p.validate(); err != nil {
		return err
	}
	if !p.Enabled {
		return nil
	}
	devices, err := b.Devices(ctx)
	if err != nil {
		return err
	}
	for _, d := range devices {
		if p.Device.matches(d) && d.Connected {
			if d.Claimed {
				return nil
			}
			return b.Attach(ctx, d)
		}
	}
	return fmt.Errorf("saved USB device %s is not connected at its selected port", p.Device.Name)
}

func loadUSBPreferences(dir string) (usbPreferences, error) {
	defaults := usbPreferences{SchemaVersion: 1}
	data, err := os.ReadFile(filepath.Join(dir, usbPreferencesFilename))
	if os.IsNotExist(err) {
		return defaults, nil
	}
	if err != nil {
		return defaults, err
	}
	if len(data) > 4096 {
		return defaults, fmt.Errorf("USB preferences are too large")
	}
	var p usbPreferences
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err = d.Decode(&p); err != nil {
		return defaults, err
	}
	if d.Decode(&struct{}{}) != io.EOF || p.SchemaVersion != 1 {
		return defaults, fmt.Errorf("invalid USB preferences")
	}
	if err := p.validate(); err != nil {
		return defaults, err
	}
	return p, nil
}

func saveUSBPreferences(dir string, p usbPreferences) error {
	p.SchemaVersion = 1
	if err := p.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".usb-preferences-*")
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
	return os.Rename(f.Name(), filepath.Join(dir, usbPreferencesFilename))
}
