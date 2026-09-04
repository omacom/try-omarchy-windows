package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRuntimeReceiptTracksReleaseAndExecutable(t *testing.T) {
	root := t.TempDir()
	bin := filepath.Join(root, "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(bin, "qemu-system-x86_64w.exe")
	if err := os.WriteFile(exe, []byte("runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	manifestSHA := strings.Repeat("a", 64)
	archiveSHA := strings.Repeat("b", 64)
	release := "https://example.test/v0.0.7-preview"
	if err := writeRuntimeReceipt(root, release, manifestSHA, archiveSHA); err != nil {
		t.Fatal(err)
	}
	if !runtimeReceiptMatches(root, release, manifestSHA, archiveSHA) {
		t.Fatal("fresh runtime receipt did not match")
	}
	if runtimeReceiptMatches(root, "https://example.test/v0.0.8-preview", manifestSHA, archiveSHA) {
		t.Fatal("runtime receipt matched another release")
	}
	if err := os.WriteFile(exe, []byte("changed runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	if runtimeReceiptMatches(root, release, manifestSHA, archiveSHA) {
		t.Fatal("runtime receipt survived an executable change")
	}
}

func TestRuntimeReceiptSurvivesRepositoryTransfer(t *testing.T) {
	root := t.TempDir()
	bin := filepath.Join(root, "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bin, "qemu-system-x86_64w.exe"), []byte("runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	manifestSHA := strings.Repeat("a", 64)
	archiveSHA := strings.Repeat("b", 64)
	tag := "v0.0.11-preview"
	if err := writeRuntimeReceipt(root, legacyReleaseBase+tag, manifestSHA, archiveSHA); err != nil {
		t.Fatal(err)
	}
	if !runtimeReceiptMatches(root, transferredReleaseBase+tag, manifestSHA, archiveSHA) {
		t.Fatal("runtime receipt did not survive the repository transfer")
	}
}
