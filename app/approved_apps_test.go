package main

import (
	"encoding/base64"
	"os"
	"strings"
	"testing"
)

func TestApprovedWindowsAppsStoreAndGuestList(t *testing.T) {
	dir := t.TempDir()
	id := strings.Repeat("a", 32)
	prefs := approvedAppPreferences{Apps: []approvedWindowsApp{{ID: id, Name: "Notes", Path: `C:\Program Files\Notes\notes.exe`}}}
	if err := saveApprovedWindowsApps(dir, prefs); err != nil {
		t.Fatal(err)
	}
	loaded, err := loadApprovedWindowsApps(dir)
	if err != nil || len(loaded.Apps) != 1 || loaded.Apps[0].Path != prefs.Apps[0].Path {
		t.Fatalf("loaded %+v, %v", loaded, err)
	}
	line, err := approvedAppsLine(loaded)
	if err != nil || !strings.HasPrefix(line, "apps ") {
		t.Fatalf("guest list: %q, %v", line, err)
	}
	data, err := base64.StdEncoding.DecodeString(strings.TrimSpace(strings.TrimPrefix(line, "apps ")))
	if err != nil || !strings.Contains(string(data), id) || !strings.Contains(string(data), "Notes") || strings.Contains(string(data), "Program Files") {
		t.Fatalf("guest list exposed path or lost app: %q, %v", data, err)
	}
	loaded.Apps = nil
	if err := saveApprovedWindowsApps(dir, loaded); err != nil {
		t.Fatal(err)
	}
	loaded, err = loadApprovedWindowsApps(dir)
	if err != nil || len(loaded.Apps) != 0 {
		t.Fatalf("revocation failed: %+v, %v", loaded, err)
	}
}

func TestApprovedWindowsAppsRejectsCorruptOrDuplicateEntries(t *testing.T) {
	dir := t.TempDir()
	id := strings.Repeat("b", 32)
	prefs := approvedAppPreferences{Apps: []approvedWindowsApp{{ID: id, Name: "One", Path: `C:\one.exe`}, {ID: id, Name: "Two", Path: `C:\two.exe`}}}
	if err := saveApprovedWindowsApps(dir, prefs); err == nil {
		t.Fatal("duplicate ID saved")
	}
	if err := os.WriteFile(approvedAppsPath(dir), []byte(`{"schemaVersion":1,"apps":[],"unknown":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadApprovedWindowsApps(dir); err == nil {
		t.Fatal("unknown field accepted")
	}
}
