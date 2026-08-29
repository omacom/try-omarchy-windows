package main

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSetupReaderStopsAfterCancellation(t *testing.T) {
	configureSetupCancellation(true)
	t.Cleanup(func() { configureSetupCancellation(false) })
	requestSetupCancel()

	buf := make([]byte, 4)
	_, err := (setupReader{r: strings.NewReader("data")}).Read(buf)
	if !errors.Is(err, errSetupCancelled) {
		t.Fatalf("read returned %v, want setup cancellation", err)
	}
	if !errors.Is(setupContext().Err(), context.Canceled) {
		t.Fatalf("request context was not cancelled: %v", setupContext().Err())
	}
}

func TestCleanupCancelledFirstInstallRemovesData(t *testing.T) {
	root := filepath.Join(t.TempDir(), "TryOmarchy")
	if err := os.MkdirAll(filepath.Join(root, "guest"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "guest", "rootfs.ext4.zst.part"), []byte("partial"), 0o644); err != nil {
		t.Fatal(err)
	}
	executable := filepath.Join(t.TempDir(), "TryOmarchy.exe")
	if err := os.WriteFile(executable, []byte("launcher"), 0o755); err != nil {
		t.Fatal(err)
	}

	if err := cleanupCancelledSetup(root, executable, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(root); !os.IsNotExist(err) {
		t.Fatalf("install data remains after cancellation: %v", err)
	}
	if _, err := os.Stat(executable); err != nil {
		t.Fatalf("launcher was removed: %v", err)
	}
}

func TestCleanupPreservesLauncherInsideInstallDirectory(t *testing.T) {
	root := filepath.Join(t.TempDir(), "TryOmarchy")
	executable := filepath.Join(root, "bin", "TryOmarchy.exe")
	if err := os.MkdirAll(filepath.Dir(executable), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(executable, []byte("launcher"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "download.part"), []byte("partial"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "bin", "stale.dll"), []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := cleanupCancelledSetup(root, executable, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(executable); err != nil {
		t.Fatalf("launcher was removed: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "download.part")); !os.IsNotExist(err) {
		t.Fatalf("partial download remains: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "bin", "stale.dll")); !os.IsNotExist(err) {
		t.Fatalf("stale runtime remains: %v", err)
	}
}

func TestCleanupExistingInstallOnlyRemovesStagingFiles(t *testing.T) {
	root := t.TempDir()
	stable := filepath.Join(root, "vm", "disk.raw")
	partial := filepath.Join(root, "guest", "rootfs.ext4.part")
	for _, path := range []string{stable, partial} {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("data"), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	if err := cleanupCancelledSetup(root, filepath.Join(t.TempDir(), "TryOmarchy.exe"), false); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(stable); err != nil {
		t.Fatalf("existing disk was removed: %v", err)
	}
	if _, err := os.Stat(partial); !os.IsNotExist(err) {
		t.Fatalf("staging file remains: %v", err)
	}
}

func TestCompleteInstallDetection(t *testing.T) {
	root := t.TempDir()
	if completeInstallExists(root) {
		t.Fatal("empty directory reported as a complete install")
	}
	for _, name := range []string{
		filepath.Join("guest", "build-spec.json"),
		filepath.Join("guest", "rootfs.ext4"),
		filepath.Join("vm", "disk.raw"),
	} {
		path := filepath.Join(root, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("data"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if !completeInstallExists(root) {
		t.Fatal("complete installation was not detected")
	}
}
