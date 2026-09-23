//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestAudioBridgeOnlyOffersUnambiguousSDLNames(t *testing.T) {
	endpoints := []audioEndpointInfo{
		{ID: "speaker-a", Name: "Speakers"},
		{ID: "speaker-b", Name: "Duplicate"},
		{ID: "speaker-c", Name: "Duplicate"},
		{ID: "speaker-d", Name: "Disconnected"},
	}
	devices := audioBridgeDevices(endpoints, []string{"Speakers", "Duplicate"})
	if len(devices) != 1 || devices[0].UID != "speaker-a" {
		t.Fatalf("unsafe audio catalog: %+v", devices)
	}
}

func TestAudioBridgeSelectionUpdatesPreferencesAndLiveRoute(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(audioRouteDirectory(root), 0700); err != nil {
		t.Fatal(err)
	}
	catalog := audioBridgeCatalog{
		Outputs: []audioBridgeDevice{{UID: "speaker-a", Name: "Speakers"}},
		Inputs:  []audioBridgeDevice{{UID: "mic-a", Name: "Microphone"}},
	}
	selection := "speaker-a"
	if err := applyAudioBridgeSelection(root, catalog, audioBridgeRequest{
		Type: "select", Direction: "output", UID: &selection,
	}, false); err != nil {
		t.Fatal(err)
	}
	names, err := loadAudioPreferences(root)
	if err != nil || names.Output != "Speakers" {
		t.Fatalf("saved output: %+v, %v", names, err)
	}
	ids, err := loadAudioEndpoints(root)
	if err != nil || ids.OutputID != selection {
		t.Fatalf("saved endpoint: %+v, %v", ids, err)
	}
	route, err := os.ReadFile(filepath.Join(audioRouteDirectory(root), "output"))
	if err != nil || strings.TrimSpace(string(route)) == "default" {
		t.Fatalf("live output route: %q, %v", route, err)
	}
	unknown := "not-in-catalog"
	if err := applyAudioBridgeSelection(root, catalog, audioBridgeRequest{
		Type: "select", Direction: "output", UID: &unknown,
	}, false); err == nil {
		t.Fatal("unapproved endpoint was accepted")
	}
	if err := saveDesktopPreferences(root, desktopPreferences{MicrophoneDisabled: true}); err != nil {
		t.Fatal(err)
	}
	mic := "mic-a"
	if err := applyAudioBridgeSelection(root, catalog, audioBridgeRequest{
		Type: "select", Direction: "input", UID: &mic,
	}, false); err == nil {
		t.Fatal("microphone-off selection was accepted")
	}
	if err := applyAudioBridgeSelection(root, catalog, audioBridgeRequest{
		Type: "select", Direction: "output",
	}, false); err != nil {
		t.Fatal(err)
	}
	route, err = os.ReadFile(filepath.Join(audioRouteDirectory(root), "output"))
	if err != nil || string(route) != "default\n" {
		t.Fatalf("default route: %q, %v", route, err)
	}
}

func TestAudioBridgeRejectsInvalidRequests(t *testing.T) {
	for _, line := range []string{
		`{"type":"select","direction":"output","deviceUID":""}`,
		`{"type":"select","direction":"camera","deviceUID":null}`,
		`{"type":"select","direction":"output","deviceUID":null,"command":"run"}`,
		`{"type":"get-catalog","direction":"output"}`,
		`{"type":"other"}`,
	} {
		if _, err := parseAudioBridgeRequest([]byte(line)); err == nil {
			t.Errorf("accepted %s", line)
		}
	}
}
