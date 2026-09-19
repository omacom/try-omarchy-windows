package main

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
)

func TestDesktopPreferencesRollbackAndBackup(t *testing.T) {
	dir := t.TempDir()
	original := settings{SchemaVersion: 1, MemoryMiB: 4096}
	if err := saveSettings(settingsPath(dir), original); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(settingsPath(dir))
	wanted := desktopPreferences{CameraDisabled: true, MicrophoneDisabled: true, CameraID: `\\?\usb#camera`, AutomaticUpdatesDisabled: true}
	if err := saveDesktopPreferences(dir, wanted); err != nil {
		t.Fatal(err)
	}
	got, err := loadDesktopPreferences(dir)
	if err != nil || !got.CameraDisabled || !got.MicrophoneDisabled || got.CameraID != wanted.CameraID || !got.AutomaticUpdatesDisabled {
		t.Fatalf("%+v %v", got, err)
	}
	after, _ := os.ReadFile(settingsPath(dir))
	if string(before) != string(after) {
		t.Fatal("new preferences modified rollback settings")
	}
	if _, err := loadSettings(settingsPath(dir)); err != nil {
		t.Fatal(err)
	}
	if !backupNameAllowed(desktopPreferencesFilename) {
		t.Fatal("backup excludes device privacy preferences")
	}
}
func TestDesktopPreferencesRejectCorruption(t *testing.T) {
	dir := t.TempDir()
	for _, data := range []string{`{"schemaVersion":2}`, `{"schemaVersion":1,"cameraDisabled":true} {}`, `{"schemaVersion":1,"unknown":true}`} {
		if err := os.WriteFile(filepath.Join(dir, desktopPreferencesFilename), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadDesktopPreferences(dir); err == nil {
			t.Fatalf("accepted corrupt preferences %s", data)
		}
	}
}
func TestMicrophoneOffKeepsPlayback(t *testing.T) {
	for _, backend := range []string{"sdl", "dsound"} {
		if got := audioBackendOptions(backend, true); got != backend+",id=snd,in.voices=0" {
			t.Fatal(got)
		}
	}
	if got := audioBackendOptions("sdl", false); got != "sdl,id=snd" {
		t.Fatal(got)
	}
}
func TestMemoryGiBConversion(t *testing.T) {
	for _, value := range []int{0, 1024, 1536, 4096, 65536} {
		got, err := memoryMiBFromGiB(memoryGiBText(value))
		if err != nil || got != strconv.Itoa(value) {
			t.Fatalf("%d: %s %v", value, got, err)
		}
	}
	for _, bad := range []string{"NaN", "+Inf", "0.5", "65", "bad"} {
		if _, err := memoryMiBFromGiB(bad); err == nil {
			t.Fatal(bad)
		}
	}
}
