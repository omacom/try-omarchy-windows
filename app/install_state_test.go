package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

func testSHA256(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func TestVerifyFileSHA256(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "artifact")
	data := []byte("verified payload")
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	var progressed int64
	ok, err := verifyFileSHA256(path, testSHA256(data), func(done, total int64) {
		progressed = done
		if total != int64(len(data)) {
			t.Errorf("progress total = %d, want %d", total, len(data))
		}
	})
	if err != nil {
		t.Fatal(err)
	}
	if !ok {
		t.Fatal("valid artifact was rejected")
	}
	if progressed != int64(len(data)) {
		t.Fatalf("progress = %d, want %d", progressed, len(data))
	}
	ok, err = verifyFileSHA256(path, testSHA256([]byte("different")), nil)
	if err != nil {
		t.Fatal(err)
	}
	if ok {
		t.Fatal("artifact with the wrong digest was accepted")
	}
}

func TestInstallReceiptTracksReleaseAndFileMetadata(t *testing.T) {
	dir := t.TempDir()
	files := map[string][]byte{
		"kernel": []byte("kernel"),
		"rootfs": []byte("root filesystem"),
	}
	names := []string{"kernel", "rootfs"}
	sums := make(map[string]string, len(files))
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
		sums[name] = testSHA256(data)
	}
	manifestSHA256 := testSHA256([]byte("manifest"))
	if err := writeInstallReceipt(dir, "https://example.test/release/", manifestSHA256, names, sums); err != nil {
		t.Fatal(err)
	}
	ok, err := installReceiptMatches(dir, "https://example.test/release", manifestSHA256, names)
	if err != nil {
		t.Fatal(err)
	}
	if !ok {
		t.Fatal("fresh receipt did not match")
	}
	ok, err = installReceiptMatches(dir, "https://example.test/other", manifestSHA256, names)
	if err != nil {
		t.Fatal(err)
	}
	if ok {
		t.Fatal("receipt matched a different release")
	}
	if err := os.WriteFile(filepath.Join(dir, "rootfs"), []byte("changed root filesystem"), 0o644); err != nil {
		t.Fatal(err)
	}
	ok, err = installReceiptMatches(dir, "https://example.test/release", manifestSHA256, names)
	if err != nil {
		t.Fatal(err)
	}
	if ok {
		t.Fatal("receipt survived changed artifact metadata")
	}
}

func TestInvalidInstallReceiptTriggersRecovery(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, installReceiptFilename), []byte("not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	ok, err := installReceiptMatches(dir, "https://example.test/release", testSHA256([]byte("manifest")), []string{"rootfs"})
	if err != nil {
		t.Fatal(err)
	}
	if ok {
		t.Fatal("malformed receipt was accepted")
	}
}

func TestInstallReceiptSurvivesRepositoryTransfer(t *testing.T) {
	dir := t.TempDir()
	data := []byte("root filesystem")
	if err := os.WriteFile(filepath.Join(dir, "rootfs"), data, 0o644); err != nil {
		t.Fatal(err)
	}
	manifest := testSHA256([]byte("manifest"))
	tag := "v0.0.11-preview"
	if err := writeInstallReceipt(dir, legacyReleaseBase+tag, manifest, []string{"rootfs"}, map[string]string{"rootfs": testSHA256(data)}); err != nil {
		t.Fatal(err)
	}
	ok, err := installReceiptMatches(dir, transferredReleaseBase+tag, manifest, []string{"rootfs"})
	if err != nil || !ok {
		t.Fatalf("transferred receipt = %v, %v", ok, err)
	}
	if releaseLocationsEquivalent(legacyReleaseBase+tag, transferredReleaseBase+"v0.0.12-preview") {
		t.Fatal("different release tags were treated as equivalent")
	}
}
