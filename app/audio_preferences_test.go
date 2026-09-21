package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestAudioPreferencesRoundTripAndRollback(t *testing.T) {
	dir := t.TempDir()
	if p, err := loadAudioPreferences(dir); err != nil || p.Output != "" || p.Input != "" {
		t.Fatalf("%+v %v", p, err)
	}
	want := audioPreferences{1, "Speakers, 世界 (USB)", "Microphone (2)"}
	if err := saveAudioPreferences(dir, want); err != nil {
		t.Fatal(err)
	}
	if got, err := loadAudioPreferences(dir); err != nil || got != want {
		t.Fatalf("%+v %v", got, err)
	}
	if _, err := loadDesktopPreferences(dir); err != nil {
		t.Fatal("audio choices broke older preferences", err)
	}
	if !backupNameAllowed(audioPreferencesFilename) {
		t.Fatal("audio choices excluded from backups")
	}
	for _, bad := range []string{`{}`, `null`, `{"schemaVersion":2}`, `{"schemaVersion":1,"input":"bad\u0000name"}`, `{"schemaVersion":1} {}`, `{"schemaVersion":1,"unknown":true}`, strings.Repeat(" ", 16385)} {
		if err := os.WriteFile(filepath.Join(dir, audioPreferencesFilename), []byte(bad), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadAudioPreferences(dir); err == nil {
			t.Fatalf("accepted corrupt preferences: %.80s", bad)
		}
	}
}

func TestAudioEnvironmentSeparatesDirectionsAndPrivacy(t *testing.T) {
	env := []string{"Path=example", "sdl_audio_device_name=wrong", "OMARCHY_SDL_OUTPUT_DEVICE_NAME=inherited", "omarchy_sdl_input_device_name=inherited"}
	p := audioPreferences{1, "Speakers, 世界", "Mic=USB"}
	for _, tc := range []struct {
		enabled, muted bool
		want           []string
	}{
		{true, false, []string{"Path=example", "OMARCHY_SDL_OUTPUT_DEVICE_NAME=Speakers, 世界", "OMARCHY_SDL_INPUT_DEVICE_NAME=Mic=USB"}},
		{true, true, []string{"Path=example", "OMARCHY_SDL_OUTPUT_DEVICE_NAME=Speakers, 世界"}},
		{false, false, []string{"Path=example"}},
	} {
		if got := audioEnvironment(env, p, tc.enabled, tc.muted); !reflect.DeepEqual(got, tc.want) {
			t.Fatalf("got %v want %v", got, tc.want)
		}
	}
	if got := audioEnvironment(env, audioPreferences{}, true, false); !reflect.DeepEqual(got, []string{"Path=example"}) {
		t.Fatal(got)
	}
}

func TestAudioSelectionRequiresRuntimeSupport(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "bin", "qemu.exe")
	if audioRuntimeSupportsSelection(exe) {
		t.Fatal("stock runtime accepted")
	}
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0755); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		data string
		want bool
	}{
		{`{"qemu":{"patches":[]}}`, false},
		{`{"qemu":{"patches":[{"file":"patches/qemu/0013-select-sdl-audio-devices.patch"}]}}`, true},
		{`broken`, false},
	} {
		if err := os.WriteFile(filepath.Join(dir, "provenance", "sources.lock.json"), []byte(tc.data), 0600); err != nil {
			t.Fatal(err)
		}
		if audioRuntimeSupportsSelection(exe) != tc.want {
			t.Fatal(tc)
		}
	}
}
