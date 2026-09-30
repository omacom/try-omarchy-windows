package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSavedUSBSelectionUsesCurrentAddressAndExactPort(t *testing.T) {
	devices, _ := parseUSBHostDevices(usbSample)
	p := usbPreferences{SchemaVersion: 1, Enabled: true, Device: selectionForUSB(devices[0])}
	f := &usbFake{objects: []usbQOMEntry{{Name: usbControllerID, Type: "child<qemu-xhci>"}}, inventory: strings.Replace(usbSample, "Addr 3", "Addr 8", 1)}
	if err := (usbBroker{f}).AttachSaved(context.Background(), p); err != nil {
		t.Fatal(err)
	}
	if f.added["hostaddr"] != 8 || f.added["auto-reconnect"] != false {
		t.Fatal(f.added)
	}
	for _, change := range []struct{ old, new string }{{"Port 2.4", "Port 2.5"}, {"1234:5678", "1234:5679"}} {
		f := &usbFake{inventory: strings.Replace(usbSample, change.old, change.new, 1)}
		if err := (usbBroker{f}).AttachSaved(context.Background(), p); err == nil || f.added != nil {
			t.Fatal("claimed a replacement", err, f.added)
		}
	}
	f = &usbFake{inventory: usbSample, failAttach: true, objects: []usbQOMEntry{{Name: usbControllerID, Type: "child<qemu-xhci>"}}}
	if err := (usbBroker{f}).AttachSaved(context.Background(), p); err == nil {
		t.Fatal("accepted busy device")
	}
	p.Enabled = false
	f = &usbFake{}
	if err := (usbBroker{f}).AttachSaved(context.Background(), p); err != nil || len(f.calls) != 0 {
		t.Fatal("disabled grant contacted runtime", err, f.calls)
	}
}
func TestUSBPreferencesRetainDisconnectedChoiceAndRollbackCompatibility(t *testing.T) {
	dir := t.TempDir()
	devices, _ := parseUSBHostDevices(usbSample)
	p := usbPreferences{Enabled: true, Device: selectionForUSB(devices[0])}
	if err := saveUSBPreferences(dir, p); err != nil {
		t.Fatal(err)
	}
	got, err := loadUSBPreferences(dir)
	if err != nil || !got.Enabled || !got.Device.matches(devices[0]) {
		t.Fatal(got, err)
	}
	choices := usbSelectionChoices(nil, got.Device)
	if len(choices) != 1 || choices[0].Connected || !got.Device.matches(choices[0]) {
		t.Fatal(choices)
	}
	if choices := usbSelectionChoices(devices, got.Device); len(choices) != 1 {
		t.Fatal("duplicated connected selection", choices)
	}
	got.Enabled = false
	if err := saveUSBPreferences(dir, got); err != nil {
		t.Fatal(err)
	}
	got, err = loadUSBPreferences(dir)
	if err != nil || got.Enabled || got.Device == nil {
		t.Fatal(got, err)
	}
	// Grant storage cannot make an older settings parser reject rollback.
	if _, err := loadSettings(settingsPath(dir)); err != nil {
		t.Fatal(err)
	}
	if !backupNameAllowed(usbPreferencesFilename) {
		t.Fatal("backup omits USB preferences")
	}
	for _, bad := range []string{`{"schemaVersion":1,"enabled":true}`, `{"schemaVersion":1,"future":true}`, `{"schemaVersion":1} {}`, `{"schemaVersion":1,"device":{"port":"../bad"}}`} {
		if err := os.WriteFile(filepath.Join(dir, usbPreferencesFilename), []byte(bad), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadUSBPreferences(dir); err == nil {
			t.Fatal("accepted malformed grant", bad)
		}
	}
}
