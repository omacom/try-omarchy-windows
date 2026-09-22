package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestAudioEndpointsRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if err := saveAudioEndpoints(dir, audioEndpoints{OutputID: "out-id", InputID: "in-id"}); err != nil {
		t.Fatal(err)
	}
	got, err := loadAudioEndpoints(dir)
	if err != nil {
		t.Fatal(err)
	}
	if got.OutputID != "out-id" || got.InputID != "in-id" {
		t.Fatalf("round trip mismatch: %+v", got)
	}
	if err := saveAudioEndpoints(dir, audioEndpoints{}); err != nil {
		t.Fatal(err)
	}
	got, err = loadAudioEndpoints(dir)
	if err != nil {
		t.Fatal(err)
	}
	if got.OutputID != "" || got.InputID != "" {
		t.Fatalf("expected empty selection, got %+v", got)
	}
}

func TestSaveAudioSelectionKeepsNamesAndIDsTogether(t *testing.T) {
	dir := t.TempDir()
	names := audioPreferences{Output: "Speakers", Input: "Microphone"}
	endpoints := audioEndpoints{OutputID: "out-id", InputID: "in-id"}
	if err := saveAudioSelection(dir, names, endpoints); err != nil {
		t.Fatal(err)
	}
	gotNames, err := loadAudioPreferences(dir)
	if err != nil {
		t.Fatal(err)
	}
	gotEndpoints, err := loadAudioEndpoints(dir)
	if err != nil {
		t.Fatal(err)
	}
	if gotNames.Output != names.Output || gotNames.Input != names.Input ||
		gotEndpoints.OutputID != endpoints.OutputID || gotEndpoints.InputID != endpoints.InputID {
		t.Fatalf("saved names=%+v endpoints=%+v", gotNames, gotEndpoints)
	}
	if !backupNameAllowed(audioEndpointsFilename) {
		t.Fatal("stable audio IDs excluded from backups")
	}
}

func TestAudioEndpointsRejectCorruption(t *testing.T) {
	dir := t.TempDir()
	if err := writeFile(filepath.Join(dir, audioEndpointsFilename), []byte(`{"schemaVersion":2}`)); err != nil {
		t.Fatal(err)
	}
	if _, err := loadAudioEndpoints(dir); err == nil {
		t.Fatal("expected version rejection")
	}
	if err := writeFile(filepath.Join(dir, audioEndpointsFilename), []byte(`{"schemaVersion":1,"outputId":"a"}{"x":1}`)); err != nil {
		t.Fatal(err)
	}
	if _, err := loadAudioEndpoints(dir); err == nil {
		t.Fatal("expected trailing data rejection")
	}
}

func TestResolveAudioSelectionPrefersStableID(t *testing.T) {
	live := []audioEndpointInfo{
		{ID: "id-a", Name: "Speakers"},
		{ID: "id-b", Name: "Headphones"},
	}
	name, id := resolveAudioSelection("Headphones (old name)", "id-a", live)
	if name != "Speakers" || id != "id-a" {
		t.Fatalf("expected id-a renamed to Speakers, got %q %q", name, id)
	}
	// Disconnected endpoint: keep the remembered name and ID for later.
	name, id = resolveAudioSelection("USB Mic", "id-gone", live)
	if name != "USB Mic" || id != "id-gone" {
		t.Fatalf("expected fallback to remembered device, got %q %q", name, id)
	}
	// Name-only selection upgrades to the live ID when the name matches.
	name, id = resolveAudioSelection("Headphones", "", live)
	if name != "Headphones" || id != "id-b" {
		t.Fatalf("expected name match to adopt id-b, got %q %q", name, id)
	}
	// Nothing remembered keeps Windows defaults.
	name, id = resolveAudioSelection("", "", live)
	if name != "" || id != "" {
		t.Fatalf("expected defaults, got %q %q", name, id)
	}
	duplicates := append(live, audioEndpointInfo{ID: "id-c", Name: "Headphones"})
	name, id = resolveAudioSelection("Headphones", "", duplicates)
	if name != "Headphones" || id != "" {
		t.Fatalf("ambiguous name adopted ID %q", id)
	}
}

func TestEndpointIDForSelection(t *testing.T) {
	live := []audioEndpointInfo{
		{ID: "out-a", Name: "Speakers"},
		{ID: "out-b", Name: "Headphones"},
	}
	for _, tc := range []struct {
		name, rememberedName, rememberedID, want string
	}{
		{"", "Speakers", "out-a", ""},
		{"Speakers", "Speakers", "out-a", "out-a"},
		{"Headphones", "Speakers", "out-a", "out-b"},
		{"Disconnected USB", "Disconnected USB", "gone", "gone"},
		{"Missing", "Speakers", "out-a", ""},
	} {
		if got := endpointIDForSelection(tc.name, tc.rememberedName, tc.rememberedID, live); got != tc.want {
			t.Fatalf("selection %q got %q want %q", tc.name, got, tc.want)
		}
	}
	duplicates := append(live, audioEndpointInfo{ID: "out-c", Name: "Headphones"})
	if got := endpointIDForSelection("Headphones", "Speakers", "out-a", duplicates); got != "" {
		t.Fatalf("ambiguous friendly name mapped to %q", got)
	}
}

func writeFile(path string, data []byte) error {
	return os.WriteFile(path, data, 0600)
}
