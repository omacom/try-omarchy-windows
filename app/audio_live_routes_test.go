package main

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLiveAudioRouteFiles(t *testing.T) {
	root := t.TempDir()
	dir := audioRouteDirectory(root)
	p := audioPreferences{SchemaVersion: 1, Output: "Speakers, USB", Input: "Microphone 🎙"}
	if err := publishAudioRoutes(dir, p, false); err != nil {
		t.Fatal(err)
	}
	read := func(direction string) string {
		t.Helper()
		data, err := os.ReadFile(filepath.Join(dir, direction))
		if err != nil {
			t.Fatal(err)
		}
		return strings.TrimSpace(string(data))
	}
	if got := read("output"); got != base64.StdEncoding.EncodeToString([]byte(p.Output)) {
		t.Fatalf("output route = %q", got)
	}
	if got := read("input"); got != base64.StdEncoding.EncodeToString([]byte(p.Input)) {
		t.Fatalf("input route = %q", got)
	}
	if err := publishSavedAudioRoutes(root, audioPreferences{SchemaVersion: 1}, true); err != nil {
		t.Fatal(err)
	}
	if got := read("output"); got != "default" {
		t.Fatalf("default output route = %q", got)
	}
	if got := read("input"); got != "default" {
		t.Fatalf("disabled microphone route = %q", got)
	}
	if err := writeAudioRoute(dir, "../other", "x"); err == nil {
		t.Fatal("invalid route direction accepted")
	}
}

func TestSaveAudioRouteDoesNotCreateControlDirectory(t *testing.T) {
	root := t.TempDir()
	if err := publishSavedAudioRoutes(root, audioPreferences{SchemaVersion: 1}, false); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(audioRouteDirectory(root)); !os.IsNotExist(err) {
		t.Fatalf("unexpected audio route directory: %v", err)
	}
}
