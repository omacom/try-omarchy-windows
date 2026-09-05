package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func portableResetFixture(t *testing.T) *config {
	t.Helper()
	cfg := resetFixture(t)
	cfg.portable = true
	cfg.diskFormat = "qcow2"
	cfg.disk = filepath.Join(cfg.vmDir, "disk.qcow2")
	if err := os.WriteFile(cfg.disk, []byte("personal USB files"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writePortableBackingState(cfg.disk, testSHA256([]byte("older factory"))); err != nil {
		t.Fatal(err)
	}
	writePortableGuestReceipt(t, cfg.guestDir, []byte("new factory"))
	cfg.fresh = true
	return cfg
}

func TestPortableResetRetainsDiskAndIdentity(t *testing.T) {
	cfg := portableResetFixture(t)
	oldIdentity, err := os.ReadFile(portableBackingStatePath(cfg.disk))
	if err != nil {
		t.Fatal(err)
	}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	retained, err := filepath.Glob(filepath.Join(cfg.vmDir, "before-reset-*", "disk.qcow2"))
	if err != nil || len(retained) != 1 {
		t.Fatalf("retained: %v %v", retained, err)
	}
	old, err := os.ReadFile(retained[0])
	if err != nil || string(old) != "personal USB files" {
		t.Fatalf("previous disk: %q %v", old, err)
	}
	identity, err := os.ReadFile(portableBackingStatePath(retained[0]))
	if err != nil || !bytes.Equal(identity, oldIdentity) {
		t.Fatalf("previous identity: %q %v", identity, err)
	}
	if ok, err := qcow2OverlayMatches(cfg.disk, "../guest/rootfs.ext4", 1<<20); err != nil || !ok {
		t.Fatalf("replacement disk: %t %v", ok, err)
	}
	if ok, err := portableBackingStateMatches(cfg.disk, testSHA256([]byte("new factory"))); err != nil || !ok {
		t.Fatalf("replacement identity: %t %v", ok, err)
	}
}

func TestPortableResetFailureKeepsOldPair(t *testing.T) {
	for _, mode := range []string{"missing-receipt", "cancelled", "invalid-size", "locked"} {
		t.Run(mode, func(t *testing.T) {
			cfg := portableResetFixture(t)
			size := int64(1)
			var lock *os.File
			switch mode {
			case "missing-receipt":
				os.RemoveAll(cfg.guestDir)
			case "cancelled":
				requestSetupCancel()
				t.Cleanup(func() { configureSetupCancellation(false) })
			case "invalid-size":
				size = -1
			case "locked":
				var err error
				lock, err = openBackupDisk(cfg.disk)
				if err != nil {
					t.Fatal(err)
				}
				defer lock.Close()
			}
			if err := prepareDisk(cfg, size); err == nil {
				t.Fatal("reset unexpectedly succeeded")
			}
			if lock != nil {
				lock.Close()
			}
			old, err := os.ReadFile(cfg.disk)
			if err != nil || string(old) != "personal USB files" {
				t.Fatalf("previous disk changed: %q %v", old, err)
			}
			if ok, err := portableBackingStateMatches(cfg.disk, testSHA256([]byte("older factory"))); err != nil || !ok {
				t.Fatalf("previous identity changed: %t %v", ok, err)
			}
		})
	}
}

func TestPortableResetHandlesMissingDiskOrIdentity(t *testing.T) {
	for _, missing := range []string{"disk", "identity"} {
		t.Run(missing, func(t *testing.T) {
			cfg := portableResetFixture(t)
			path := cfg.disk
			if missing == "identity" {
				path = portableBackingStatePath(cfg.disk)
			}
			if err := os.Remove(path); err != nil {
				t.Fatal(err)
			}
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal(err)
			}
			if ok, err := portableBackingStateMatches(cfg.disk, testSHA256([]byte("new factory"))); err != nil || !ok {
				t.Fatalf("replacement identity: %t %v", ok, err)
			}
		})
	}
}

func TestPortableResetRollsBackEveryPublicationFailure(t *testing.T) {
	for fail := 0; fail < 4; fail++ {
		t.Run(string(rune('0'+fail)), func(t *testing.T) {
			root := t.TempDir()
			disk, identity, newDisk, newIdentity, oldDisk, oldIdentity := filepath.Join(root, "disk"), filepath.Join(root, "identity"), filepath.Join(root, "new-disk"), filepath.Join(root, "new-identity"), filepath.Join(root, "old-disk"), filepath.Join(root, "old-identity")
			for path, value := range map[string]string{disk: "old disk", identity: "old identity", newDisk: "new disk", newIdentity: "new identity"} {
				if err := os.WriteFile(path, []byte(value), 0600); err != nil {
					t.Fatal(err)
				}
			}
			moves := []resetMove{{disk, oldDisk}, {identity, oldIdentity}, {newIdentity, identity}, {newDisk, disk}}
			count := 0
			err := publishPortableReset(moves, func(from, to string) error {
				count++
				if count == fail+1 {
					return errors.New("publication failed")
				}
				return os.Rename(from, to)
			})
			if err == nil {
				t.Fatal("expected failure")
			}
			for path, want := range map[string]string{disk: "old disk", identity: "old identity"} {
				got, err := os.ReadFile(path)
				if err != nil || string(got) != want {
					t.Fatalf("%s: %q %v", path, got, err)
				}
			}
		})
	}
}

func TestPortableResetFailedRollbackDoesNotPublishMismatchedPair(t *testing.T) {
	root := t.TempDir()
	disk, identity, newDisk, newIdentity, oldDisk, oldIdentity := filepath.Join(root, "disk"), filepath.Join(root, "identity"), filepath.Join(root, "new-disk"), filepath.Join(root, "new-identity"), filepath.Join(root, "old-disk"), filepath.Join(root, "old-identity")
	for path, value := range map[string]string{disk: "old disk", identity: "old identity", newDisk: "new disk", newIdentity: "new identity"} {
		if err := os.WriteFile(path, []byte(value), 0600); err != nil {
			t.Fatal(err)
		}
	}
	err := publishPortableReset([]resetMove{{disk, oldDisk}, {identity, oldIdentity}, {newIdentity, identity}, {newDisk, disk}}, func(from, to string) error {
		if from == newDisk || from == identity && to == newIdentity {
			return errors.New("file is locked")
		}
		return os.Rename(from, to)
	})
	if err == nil || !strings.Contains(err.Error(), identity) {
		t.Fatalf("missing recovery error: %v", err)
	}
	if _, err := os.Stat(disk); !os.IsNotExist(err) {
		t.Fatal("old disk exposed with replacement identity")
	}
	for path, want := range map[string]string{oldDisk: "old disk", oldIdentity: "old identity"} {
		got, err := os.ReadFile(path)
		if err != nil || string(got) != want {
			t.Fatalf("lost retained file: %q %v", got, err)
		}
	}
}
