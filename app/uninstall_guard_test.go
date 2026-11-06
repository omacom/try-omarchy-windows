package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUninstallLinkedAncestorHasRecoveryGuidance(t *testing.T) {
	root := t.TempDir()
	realProfile := filepath.Join(root, "real-profile")
	realDir := filepath.Join(realProfile, "TryOmarchy")
	if err := os.MkdirAll(realDir, 0700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(realDir, provisionModeFilename)
	if err := os.WriteFile(marker, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	linkedProfile := filepath.Join(root, "linked-profile")
	if err := os.Symlink(realProfile, linkedProfile); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	dir := filepath.Join(linkedProfile, "TryOmarchy")
	err := removeUninstallDirectory(dir, func(string) error {
		t.Fatal("attempted deletion through a linked profile")
		return nil
	})
	if err == nil || !strings.Contains(err.Error(), linkedProfile) || !strings.Contains(err.Error(), "real folder path") || !strings.Contains(err.Error(), "manually") {
		t.Fatalf("missing linked-component recovery guidance: %v", err)
	}
	if err := validateUninstallDirectory(realDir); err != nil {
		t.Fatalf("real folder path rejected: %v", err)
	}
	if data, err := os.ReadFile(marker); err != nil || string(data) != "keep" {
		t.Fatalf("data changed: %q err=%v", data, err)
	}
	state := moveState{Redirects: map[string]string{filepath.Join(root, "old-install"): dir}}
	if _, err := resolveMovedDirectory(state, filepath.Join(root, "old-install"), true); err == nil || !strings.Contains(err.Error(), "manually") {
		t.Fatalf("moved uninstall lost recovery guidance: %v", err)
	}
}

func TestValidateUninstallDirectoryNeedsADataFolder(t *testing.T) {
	for _, marker := range uninstallMarkers {
		dir := t.TempDir()
		path := filepath.Join(dir, marker)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := validateUninstallDirectory(dir); err != nil {
			t.Errorf("%s: %v", marker, err)
		}
	}

	// A folder of documents, even one with the same subfolder names and a
	// downloaded launcher, is not an installation.
	docs := t.TempDir()
	for _, name := range []string{"vm", "guest", "runtime", filepath.Join("vm", "disk.raw", "inner")} {
		if err := os.MkdirAll(filepath.Join(docs, name), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{stableLauncherName, "settings.json", "notes.txt"} {
		if err := os.WriteFile(filepath.Join(docs, name), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := validateUninstallDirectory(docs); err == nil {
		t.Fatal("accepted a folder that is not a data folder")
	}

	for _, dir := range []string{
		"",
		"relative",
		docs + string(filepath.Separator) + ".." + string(filepath.Separator) + filepath.Base(docs),
		filepath.Join(docs, "missing"),
		filepath.Join(docs, "notes.txt"),
		filepath.VolumeName(docs) + string(filepath.Separator),
	} {
		if err := validateUninstallDirectory(dir); err == nil {
			t.Errorf("accepted %q", dir)
		}
	}
}

func TestUninstallRetryPreservesLauncherAndReceipt(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{provisionModeFilename, stableLauncherName, "locked.txt"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("keep"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	failure := errors.New("locked")
	err := removeUninstallDirectory(dir, func(path string) error {
		if filepath.Base(path) == "locked.txt" {
			return failure
		}
		return os.RemoveAll(path)
	})
	if !errors.Is(err, failure) {
		t.Fatalf("removal = %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, stableLauncherName)); err != nil {
		t.Fatal("lost retry launcher", err)
	}
	// A partial first attempt can remove all original installation markers.
	if err := os.Remove(filepath.Join(dir, provisionModeFilename)); err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	if err := validateUninstallDirectory(dir); err != nil {
		t.Fatal("lost guarded retry", err)
	}
	if err := removeUninstallDirectory(dir, os.RemoveAll); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("directory remains: %v", err)
	}
}

func TestUninstallRemovalRefusesUnownedAndLinkedPaths(t *testing.T) {
	docs := t.TempDir()
	note := filepath.Join(docs, "notes.txt")
	if err := os.WriteFile(note, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, dir := range []string{docs, "", "relative", filepath.Join(docs, "missing")} {
		calls := 0
		if err := removeUninstallDirectory(dir, func(string) error { calls++; return nil }); err == nil || calls != 0 {
			t.Fatalf("unsafe deletion %q: calls=%d error=%v", dir, calls, err)
		}
	}
	install := t.TempDir()
	if err := os.WriteFile(filepath.Join(install, provisionModeFilename), []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(install, link); err != nil {
		t.Skipf("symlink unavailable: %v", err)
	}
	for _, dir := range []string{link, filepath.Join(link, "nested")} {
		if err := removeUninstallDirectory(dir, func(string) error { t.Fatal("removed linked data"); return nil }); err == nil {
			t.Fatal("accepted link", dir)
		}
	}
	if err := os.WriteFile(filepath.Join(docs, uninstallStateName), []byte(`{"version":1,"directory":"elsewhere"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := validateUninstallDirectory(docs); err == nil {
		t.Fatal("accepted foreign receipt")
	}
	if data, err := os.ReadFile(note); err != nil || string(data) != "keep" {
		t.Fatal("lost documents")
	}
}

func TestUninstallRejectsLinkedMarkerAndReceipt(t *testing.T) {
	dir := t.TempDir()
	outside := t.TempDir()
	if err := os.WriteFile(filepath.Join(outside, "disk.raw"), []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(dir, "vm")); err != nil {
		t.Skipf("symlink unavailable: %v", err)
	}
	if err := validateUninstallDirectory(dir); err == nil {
		t.Fatal("accepted linked marker")
	}
	if err := os.WriteFile(filepath.Join(dir, provisionModeFilename), []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	receipt := filepath.Join(outside, "receipt")
	if err := os.WriteFile(receipt, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(receipt, filepath.Join(dir, uninstallStateName)); err != nil {
		t.Fatal(err)
	}
	if err := removeUninstallDirectory(dir, func(string) error { t.Fatal("removed data despite linked receipt"); return nil }); err == nil {
		t.Fatal("accepted linked receipt")
	}
	if data, _ := os.ReadFile(receipt); string(data) != "keep" {
		t.Fatal("overwrote link target")
	}
}

func TestUninstallBootstrapPreservesOtherInstallAndUnknownFiles(t *testing.T) {
	for _, other := range []bool{false, true} {
		t.Run(fmt.Sprint(other), func(t *testing.T) {
			root := t.TempDir()
			def, dir := filepath.Join(root, "default"), filepath.Join(root, "moved")
			pointed := dir
			if other {
				pointed = filepath.Join(root, "other")
			}
			if err := saveDataLocationPointer(def, pointed); err != nil {
				t.Fatal(err)
			}
			archive := filepath.Join(def, "diagnostics", "try-omarchy-diagnostics-20261005-120000.123456789.zip")
			unknown := filepath.Join(def, "diagnostics", "my-backup.zip")
			host := filepath.Join(def, "portable-host", "keep.txt")
			for _, path := range []string{archive, unknown, host} {
				if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, []byte("keep"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			if err := cleanupUninstallBootstrap(def, dir); err != nil {
				t.Fatal(err)
			}
			for _, path := range []string{unknown, host} {
				if data, err := os.ReadFile(path); err != nil || string(data) != "keep" {
					t.Fatal("lost unrelated data", path)
				}
			}
			_, err := os.Stat(archive)
			if other && err != nil {
				t.Fatal("removed another install's diagnostics")
			}
			if !other && !os.IsNotExist(err) {
				t.Fatal("left owned diagnostics")
			}
			_, found, err := loadDataLocationPointer(def)
			if err != nil || found != other {
				t.Fatalf("pointer found=%v err=%v", found, err)
			}
		})
	}
}

func TestUninstallRemovesHostApprovals(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{provisionModeFilename, approvedAppsFilename, approvedURLSchemesFilename, approvedFileTypesFilename} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("fixture"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := removeUninstallDirectory(dir, os.RemoveAll); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("installation or approvals remain: %v", err)
	}
}
