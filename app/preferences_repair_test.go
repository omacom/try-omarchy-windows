package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestRepairPreferencesPreservesOriginalAndGuest(t *testing.T) {
	for _, name := range []string{settingsFileName, storageSettingsFilename} {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			file := filepath.Join(dir, name)
			original := []byte(`{"broken preferences`)
			if err := os.WriteFile(file, original, 0600); err != nil {
				t.Fatal(err)
			}
			guest := filepath.Join(dir, "disk.raw")
			if err := os.WriteFile(guest, []byte("guest files"), 0600); err != nil {
				t.Fatal(err)
			}
			saved, err := repairPreferences(file)
			if err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(saved)
			if err != nil || !bytes.Equal(before, original) {
				t.Fatalf("original preferences lost: %q %v", before, err)
			}
			if name == settingsFileName {
				value, err := loadSettings(file)
				if err != nil {
					t.Fatal(err)
				}
				if value.activeShare() != "" || len(value.Forwards) != 0 || !value.ShareDisabled || !value.SharedFolderPrompted {
					t.Fatalf("unexpected restored defaults: %+v", value)
				}
			} else {
				value, err := loadStorageSettings(dir)
				if err != nil || value.DiskGiB != 0 {
					t.Fatalf("storage defaults: %+v %v", value, err)
				}
			}
			data, err := os.ReadFile(guest)
			if err != nil || string(data) != "guest files" {
				t.Fatal("repair touched guest files")
			}
		})
	}
}

func TestRepairPreferencesWriteFailureKeepsOriginal(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, settingsFileName)
	original := []byte("broken")
	if err := os.WriteFile(file, original, 0600); err != nil {
		t.Fatal(err)
	}
	injected := errors.New("simulated write failure")
	saved, err := preserveAndRepairPreferences(file, func() error { return injected })
	if !errors.Is(err, injected) {
		t.Fatalf("write error: %v", err)
	}
	for _, path := range []string{file, saved} {
		data, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(data, original) {
			t.Fatalf("lost preferences at %s", path)
		}
	}
}

func TestRepairPreferencesRejectsUnexpectedFiles(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "disk.raw")
	if err := os.WriteFile(file, []byte("guest"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := repairPreferences(file); err == nil {
		t.Fatal("accepted a disk as preferences")
	}
	folder := filepath.Join(dir, settingsFileName)
	if err := os.Mkdir(folder, 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := repairPreferences(folder); err == nil {
		t.Fatal("accepted a directory as preferences")
	}
	data, _ := os.ReadFile(file)
	if string(data) != "guest" {
		t.Fatal("changed guest data")
	}
}
