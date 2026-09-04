package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLauncherUpdateStateRoundTrip(t *testing.T) {
	dir := t.TempDir()
	want := &launcherUpdateState{
		Schema: updateStateVersion, Version: "v0.0.7-preview",
		SHA256: strings.Repeat("a", 64), Started: true, HasPrevious: true,
	}
	if err := writeLauncherUpdateState(dir, want); err != nil {
		t.Fatal(err)
	}
	got, err := readLauncherUpdateState(dir)
	if err != nil {
		t.Fatal(err)
	}
	if *got != *want {
		t.Fatalf("state = %#v, want %#v", got, want)
	}
	if err := clearLauncherUpdateState(dir); err != nil {
		t.Fatal(err)
	}
	if got, err := readLauncherUpdateState(dir); err != nil || got != nil {
		t.Fatalf("state after clear = %#v, %v", got, err)
	}
}

func TestUpdateCheckThrottle(t *testing.T) {
	dir := t.TempDir()
	now := time.Unix(1_800_000_000, 0)
	if !updateCheckDue(dir, now) {
		t.Fatal("first update check was throttled")
	}
	if err := recordUpdateCheck(dir, now); err != nil {
		t.Fatal(err)
	}
	if updateCheckDue(dir, now.Add(updateCheckInterval-time.Second)) {
		t.Fatal("recent update check was not throttled")
	}
	if !updateCheckDue(dir, now.Add(updateCheckInterval)) {
		t.Fatal("expired update check was throttled")
	}
}

func TestRestartArgumentsRoundTrip(t *testing.T) {
	want := []string{"-dir", `C:\Users\Test User\TryOmarchy`, "-fullscreen"}
	encoded, err := encodeRestartArgs(want)
	if err != nil {
		t.Fatal(err)
	}
	got, err := decodeRestartArgs(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(want) {
		t.Fatalf("arguments = %#v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("argument %d = %q, want %q", i, got[i], want[i])
		}
	}
}

func TestLauncherUpdateStateRejectsMalformedFile(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, updateStateFilename), []byte(`{"schema":1}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := readLauncherUpdateState(dir); err == nil {
		t.Fatal("malformed state was accepted")
	}
}

func TestUpdatePathsStayInsideInstall(t *testing.T) {
	dir := t.TempDir()
	for _, path := range []string{launcherUpdateDir(dir), previousLauncherPath(dir), stagedLauncherPath(dir, "v0.0.7-preview")} {
		rel, err := filepath.Rel(dir, path)
		if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
			t.Fatalf("path escaped install: %q", path)
		}
	}
}

func TestStableRecoveryStateSurvivesRestart(t *testing.T) {
	dir := t.TempDir()
	launcher := &launcherUpdateState{Schema: updateStateVersion, Version: "v1.0.0",
		SHA256: strings.Repeat("a", 64), Started: true, HasPrevious: true}
	if err := writeLauncherUpdateState(dir, launcher); err != nil {
		t.Fatal(err)
	}
	got, err := readLauncherUpdateState(dir)
	if err != nil || got == nil || *got != *launcher {
		t.Fatalf("launcher state = %+v, %v", got, err)
	}
	if err := recordPayloadUpdate(dir, "v1.0.0", true, true); err != nil {
		t.Fatal(err)
	}
	payload, err := readPayloadUpdateState(dir)
	if err != nil || payload == nil || payload.Version != "v1.0.0" || !payload.GuestPending || !payload.RuntimePending {
		t.Fatalf("payload state = %+v, %v", payload, err)
	}
}
