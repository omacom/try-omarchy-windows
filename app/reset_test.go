package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func resetFixture(t *testing.T) *config {
	t.Helper()
	configureSetupCancellation(false)
	dir := t.TempDir()
	cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest"), vmDir: filepath.Join(dir, "vm"), disk: filepath.Join(dir, "vm", "disk.raw"), diskFormat: "raw"}
	for _, folder := range []string{cfg.guestDir, cfg.vmDir} {
		if err := os.MkdirAll(folder, 0700); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(cfg.disk, []byte("existing personal files"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg.guestDir, "rootfs.ext4"), []byte("factory image"), 0600); err != nil {
		t.Fatal(err)
	}
	return cfg
}

func TestResetRetainsPreviousDiskAndPublishesFactory(t *testing.T) {
	cfg := resetFixture(t)
	old, err := resetStandardDisk(cfg, 1)
	if err != nil {
		t.Fatal(err)
	}
	previous, err := os.ReadFile(old)
	if err != nil || string(previous) != "existing personal files" {
		t.Fatalf("retained disk: %q, %v", previous, err)
	}
	current, err := os.ReadFile(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	if len(current) != 1<<20 || !bytes.HasPrefix(current, []byte("factory image")) {
		t.Fatal("replacement disk is incomplete")
	}
	leftovers, _ := filepath.Glob(filepath.Join(cfg.vmDir, ".reset-staging-*"))
	if len(leftovers) != 0 {
		t.Fatal("left staging files")
	}
}

func TestResetFailuresPreserveOriginal(t *testing.T) {
	for _, mode := range []string{"missing-factory", "low-space", "cancelled", "locked", "pending-update", "invalid-size", "oversized-factory"} {
		t.Run(mode, func(t *testing.T) {
			cfg := resetFixture(t)
			size := int64(1)
			saved := diskFreeBytes
			t.Cleanup(func() { diskFreeBytes = saved; configureSetupCancellation(false) })
			switch mode {
			case "missing-factory":
				os.Remove(filepath.Join(cfg.guestDir, "rootfs.ext4"))
			case "low-space":
				diskFreeBytes = func(string) (int64, error) { return 0, nil }
			case "cancelled":
				requestSetupCancel()
			case "locked":
				lock, err := openBackupDisk(cfg.disk)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { lock.Close() })
			case "pending-update":
				if err := os.WriteFile(filepath.Join(cfg.dir, payloadUpdateStateFilename), []byte("pending"), 0600); err != nil {
					t.Fatal(err)
				}
			case "invalid-size":
				size = -1
			case "oversized-factory":
				if err := os.WriteFile(filepath.Join(cfg.guestDir, "rootfs.ext4"), make([]byte, 2<<20), 0600); err != nil {
					t.Fatal(err)
				}
			}
			_, err := resetStandardDisk(cfg, size)
			if err == nil {
				t.Fatal("reset succeeded")
			}
			if mode == "low-space" && !errors.Is(err, errInsufficientDiskSpace) {
				t.Fatalf("unexpected space failure: %v", err)
			}
			// Release the test's exclusive handle before reading on Windows.
			if mode == "locked" {
				return
			}
			current, readErr := os.ReadFile(cfg.disk)
			if readErr != nil || string(current) != "existing personal files" {
				t.Fatalf("original changed: %q, %v", current, readErr)
			}
		})
	}
}

func TestFreshStandardInstallUsesSafeReset(t *testing.T) {
	cfg := resetFixture(t)
	cfg.fresh = true
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	retained, _ := filepath.Glob(filepath.Join(cfg.vmDir, "before-reset-*", "disk.raw"))
	if len(retained) != 1 {
		t.Fatalf("retained disks: %v", retained)
	}
	content, _ := os.ReadFile(retained[0])
	if string(content) != "existing personal files" {
		t.Fatal("fresh lost the old disk")
	}
}

func TestResetPublicationFailurePreservesRecovery(t *testing.T) {
	for _, rollbackFails := range []bool{false, true} {
		t.Run(fmt.Sprint(rollbackFails), func(t *testing.T) {
			dir := t.TempDir()
			current := filepath.Join(dir, "disk.raw")
			staged := filepath.Join(dir, "new.raw")
			retained := filepath.Join(dir, "old.raw")
			if err := os.WriteFile(current, []byte("personal files"), 0600); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(staged, []byte("factory"), 0600); err != nil {
				t.Fatal(err)
			}
			err := publishResetDisk(current, staged, retained, func(from, to string) error {
				if from == staged || rollbackFails && from == retained {
					return errors.New("simulated publication failure")
				}
				return os.Rename(from, to)
			})
			if err == nil {
				t.Fatal("expected publication failure")
			}
			preserved := current
			if rollbackFails {
				preserved = retained
				if !strings.Contains(err.Error(), retained) {
					t.Fatal("missing recovery location")
				}
			}
			data, readErr := os.ReadFile(preserved)
			if readErr != nil || string(data) != "personal files" {
				t.Fatalf("lost previous disk: %q %v", data, readErr)
			}
		})
	}
}
