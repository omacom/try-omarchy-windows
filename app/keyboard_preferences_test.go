package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestKeyboardPreferencesRoundTrip(t *testing.T) {
	dir := t.TempDir()
	got, err := loadKeyboardPreferences(dir)
	if err != nil || got.AltTabToWindows {
		t.Fatalf("missing file = %#v, %v; want Alt+Tab forwarded", got, err)
	}
	if err := saveKeyboardPreferences(dir, keyboardPreferences{AltTabToWindows: true}); err != nil {
		t.Fatal(err)
	}
	got, err = loadKeyboardPreferences(dir)
	if err != nil || got != (keyboardPreferences{SchemaVersion: 1, AltTabToWindows: true}) {
		t.Fatalf("preferences = %#v, %v", got, err)
	}
	if err := os.WriteFile(filepath.Join(dir, keyboardPreferencesFilename), []byte(`{"schemaVersion":1,"future":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadKeyboardPreferences(dir); err == nil {
		t.Fatal("unknown keyboard preference accepted")
	}
	if !backupNameAllowed(keyboardPreferencesFilename) {
		t.Fatal("backups leave out keyboard preferences")
	}
}

func TestApplyKeyboardPreferencesKeepsBehaviorOnBadFile(t *testing.T) {
	t.Cleanup(func() { altTabToWindows.Store(false) })
	dir := t.TempDir()
	var lastErr string
	if err := saveKeyboardPreferences(dir, keyboardPreferences{AltTabToWindows: true}); err != nil {
		t.Fatal(err)
	}
	applyKeyboardPreferences(dir, &lastErr)
	if !altTabToWindows.Load() || lastErr != "" {
		t.Fatalf("saved choice not applied: %v, %q", altTabToWindows.Load(), lastErr)
	}
	if err := os.WriteFile(filepath.Join(dir, keyboardPreferencesFilename), []byte(`{`), 0600); err != nil {
		t.Fatal(err)
	}
	applyKeyboardPreferences(dir, &lastErr)
	if !altTabToWindows.Load() || lastErr == "" {
		t.Fatalf("unreadable file changed behavior: %v, %q", altTabToWindows.Load(), lastErr)
	}
	if err := os.Remove(filepath.Join(dir, keyboardPreferencesFilename)); err != nil {
		t.Fatal(err)
	}
	applyKeyboardPreferences(dir, &lastErr)
	if altTabToWindows.Load() || lastErr != "" {
		t.Fatalf("removed file kept Alt+Tab on Windows: %v, %q", altTabToWindows.Load(), lastErr)
	}
}
