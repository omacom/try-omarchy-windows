package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestLaunchPreferencesRoundTrip(t *testing.T) {
	dir := t.TempDir()
	want := launchPreferences{SchemaVersion: 1, StartAutomatically: true, LaunchAtSignIn: true}
	if err := saveLaunchPreferences(dir, want); err != nil {
		t.Fatal(err)
	}
	got, err := loadLaunchPreferences(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("preferences = %#v, want %#v", got, want)
	}
	if err := os.WriteFile(filepath.Join(dir, launchPreferencesFilename), []byte(`{"schemaVersion":1,"future":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadLaunchPreferences(dir); err == nil {
		t.Fatal("unknown launch preference accepted")
	}
}
