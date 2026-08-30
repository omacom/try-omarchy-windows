package main

import (
	"os"
	"path/filepath"
	"testing"
)

func writeVersionFile(t *testing.T, dir, value string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "version"), []byte(value), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readVersionFile(t *testing.T, dir string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(dir, "version"))
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestDirectoryUpdatePublishesAndRollsBack(t *testing.T) {
	root := t.TempDir()
	current := filepath.Join(root, "guest")
	staged := filepath.Join(root, "guest.next")
	previous := filepath.Join(root, "guest.previous")
	writeVersionFile(t, current, "old")
	writeVersionFile(t, staged, "new")
	if err := publishDirectoryUpdate(current, staged, previous); err != nil {
		t.Fatal(err)
	}
	if got := readVersionFile(t, current); got != "new" {
		t.Fatalf("current = %q", got)
	}
	if got := readVersionFile(t, previous); got != "old" {
		t.Fatalf("previous = %q", got)
	}
	if err := rollbackDirectoryUpdate(current, previous, filepath.Join(root, "failed")); err != nil {
		t.Fatal(err)
	}
	if got := readVersionFile(t, current); got != "old" {
		t.Fatalf("rolled back current = %q", got)
	}
}

func TestPendingPayloadUpdateRollsBackOnSecondStart(t *testing.T) {
	root := t.TempDir()
	for _, kind := range []string{"guest", "runtime"} {
		writeVersionFile(t, filepath.Join(root, kind), "new")
		writeVersionFile(t, filepath.Join(root, kind+".previous"), "old")
	}
	state := &payloadUpdateState{
		Schema: payloadUpdateStateVersion, Version: "v0.0.7-preview",
		GuestPending: true, RuntimePending: true, Started: true,
	}
	if err := writePayloadUpdateState(root, state); err != nil {
		t.Fatal(err)
	}
	rolledBack, err := rollbackPendingPayloadUpdates(root)
	if err != nil {
		t.Fatal(err)
	}
	if !rolledBack {
		t.Fatal("pending payloads were not rolled back")
	}
	for _, kind := range []string{"guest", "runtime"} {
		if got := readVersionFile(t, filepath.Join(root, kind)); got != "old" {
			t.Fatalf("%s = %q after rollback", kind, got)
		}
	}
}
