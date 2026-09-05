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
	if completeInstallExists(root, "disk.raw") {
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
	if !completeInstallExists(root, "disk.raw") {
		t.Fatal("complete installation was not detected")
	}
	if completeInstallExists(root, "disk.qcow2") {
		t.Fatal("raw installation was reported as a portable installation")
	}
	if err := os.WriteFile(filepath.Join(root, "vm", "disk.qcow2"), []byte("data"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !completeInstallExists(root, "disk.qcow2") {
		t.Fatal("complete portable installation was not detected")
	}
}

func TestCancelPreservesUnrelatedPartialFiles(t *testing.T) {
	root := t.TempDir()
	paths := []string{"notes.part", "guest/personal.part", "vm/project.part", "Shared/movie.part", "vm/before-reset-example/disk.qcow2.part"}
	for _, name := range paths {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("keep me"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{runtimeZip + ".part", "guest/rootfs.ext4.zst.part", "guest.next/vmlinuz-linux.part", "vm/disk.qcow2.part"} {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("staging"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := cleanupCancelledSetup(root, "", false); err != nil {
		t.Fatal(err)
	}
	for _, name := range paths {
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(name)))
		if err != nil || string(data) != "keep me" {
			t.Fatalf("unrelated file %s changed: %q %v", name, data, err)
		}
	}
	for _, name := range []string{runtimeZip + ".part", "guest/rootfs.ext4.zst.part", "guest.next/vmlinuz-linux.part", "vm/disk.qcow2.part"} {
		if _, err := os.Stat(filepath.Join(root, filepath.FromSlash(name))); !os.IsNotExist(err) {
			t.Fatalf("staging file %s remains: %v", name, err)
		}
	}
}

func TestCancelDoesNotFollowGuestFolderLink(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	path := filepath.Join(external, "rootfs.ext4.part")
	if err := os.WriteFile(path, []byte("external file"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(external, filepath.Join(root, "guest")); err != nil {
		t.Skipf("symlink unavailable: %v", err)
	}
	if err := cleanupCancelledSetup(root, "", false); err != nil {
		t.Fatal(err)
	}
	if data, err := os.ReadFile(path); err != nil || string(data) != "external file" {
		t.Fatalf("external file changed: %q %v", data, err)
	}
}

func TestCancelPreservesPortableRecoveryAfterInterruptedReset(t *testing.T) {
	root := t.TempDir()
	retained := filepath.Join(root, "vm", "before-reset-example", "disk.qcow2")
	if err := os.MkdirAll(filepath.Dir(retained), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(retained, []byte("previous personal files"), 0600); err != nil {
		t.Fatal(err)
	}
	// An interrupted publication can leave no active disk. That does not make
	// the nonempty data directory disposable on the next portable launch.
	if completeInstallExists(root, "disk.qcow2") {
		t.Fatal("fixture unexpectedly complete")
	}
	removeAll, err := dataDirectoryEmpty(root)
	if err != nil {
		t.Fatal(err)
	}
	if err := cleanupCancelledSetup(root, "", removeAll); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(retained)
	if err != nil || string(data) != "previous personal files" {
		t.Fatalf("retained disk changed: %q %v", data, err)
	}
}
