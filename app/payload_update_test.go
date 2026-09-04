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

func TestFailedDirectoryPublicationKeepsRecoveryState(t *testing.T) {
	root := t.TempDir()
	current := filepath.Join(root, "guest")
	writeVersionFile(t, current, "old")
	if err := recordPayloadUpdate(root, "v0.0.9-preview", true, false); err != nil {
		t.Fatal(err)
	}
	if err := publishDirectoryUpdate(current, filepath.Join(root, "missing.next"), filepath.Join(root, "guest.previous")); err == nil {
		t.Fatal("publishing a missing staged tree succeeded")
	}
	state, err := readPayloadUpdateState(root)
	if err != nil || state == nil || !state.GuestPending {
		t.Fatalf("recovery state after publication failure = %+v, %v", state, err)
	}
	if got := readVersionFile(t, current); got != "old" {
		t.Fatalf("current after failed publication = %q", got)
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

func TestPayloadUpdateComponentsCanCommitIndependently(t *testing.T) {
	root := t.TempDir()
	for _, kind := range []string{"guest", "runtime"} {
		writeVersionFile(t, filepath.Join(root, kind), "new")
		writeVersionFile(t, filepath.Join(root, kind+".previous"), "old")
	}
	if err := recordPayloadUpdate(root, "v0.0.9-preview", true, true); err != nil {
		t.Fatal(err)
	}

	commitRuntimePayloadUpdate(root)
	if _, err := os.Stat(filepath.Join(root, "runtime.previous")); !os.IsNotExist(err) {
		t.Fatalf("runtime rollback tree remains after runtime commit: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "guest.previous")); err != nil {
		t.Fatalf("guest rollback tree removed before userspace readiness: %v", err)
	}
	state, err := readPayloadUpdateState(root)
	if err != nil || state == nil || !state.GuestPending || state.RuntimePending {
		t.Fatalf("state after runtime commit = %+v, %v", state, err)
	}

	commitGuestPayloadUpdate(root)
	if _, err := os.Stat(filepath.Join(root, "guest.previous")); !os.IsNotExist(err) {
		t.Fatalf("guest rollback tree remains after userspace readiness: %v", err)
	}
	if state, err := readPayloadUpdateState(root); err != nil || state != nil {
		t.Fatalf("state after guest commit = %+v, %v", state, err)
	}
}

func TestPinRestoredPayloadsUsesInstalledReceipts(t *testing.T) {
	root := t.TempDir()
	guest := filepath.Join(root, "guest")
	guestFiles := []string{"rootfs.ext4"}
	if err := os.MkdirAll(guest, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range guestFiles {
		if err := os.WriteFile(filepath.Join(guest, name), []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	guestManifest := testSHA256([]byte("old guest manifest"))
	guestSums := make(map[string]string, len(guestFiles))
	for _, name := range guestFiles {
		guestSums[name] = testSHA256([]byte(name))
	}
	if err := writeInstallReceipt(guest, "https://example.invalid/v0.0.8-preview", guestManifest, guestFiles, guestSums); err != nil {
		t.Fatal(err)
	}

	runtime := filepath.Join(root, "runtime")
	executable := filepath.Join(runtime, "bin", "qemu-system-x86_64w.exe")
	if err := os.MkdirAll(filepath.Dir(executable), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(executable, []byte("old runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	runtimeManifest := testSHA256([]byte("old runtime manifest"))
	if err := writeRuntimeReceipt(runtime, "https://example.invalid/runtime-old", runtimeManifest, testSHA256([]byte("archive"))); err != nil {
		t.Fatal(err)
	}

	guestRelease, guestPin := "failed", "failed"
	runtimeRelease, runtimePin := "failed", "failed"
	if err := pinRestoredPayloads(root, &guestRelease, &guestPin, &runtimeRelease, &runtimePin); err != nil {
		t.Fatal(err)
	}
	if guestRelease != "https://example.invalid/v0.0.8-preview" || guestPin != guestManifest ||
		runtimeRelease != "https://example.invalid/runtime-old" || runtimePin != runtimeManifest {
		t.Fatalf("restored pins = %q %q %q %q", guestRelease, guestPin, runtimeRelease, runtimePin)
	}
}
