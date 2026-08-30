package main

import (
	"net/http"
	"os"
	"path/filepath"
	"testing"
)

func TestPortableModeDisablesAutomaticUpdates(t *testing.T) {
	cfg := &config{portable: true}
	if automaticUpdatesEnabled(cfg, false, defaultReleaseURL, defaultSumsSHA256) {
		t.Fatal("portable mode enabled the automatic-update network path")
	}
	cfg.portable = false
	if !automaticUpdatesEnabled(cfg, false, defaultReleaseURL, defaultSumsSHA256) {
		t.Fatal("standard mode disabled authenticated automatic updates")
	}
	if automaticUpdatesEnabled(cfg, true, defaultReleaseURL, defaultSumsSHA256) {
		t.Fatal("-no-update did not disable automatic updates")
	}
}

func TestPortableManifestUsesPinnedDigestWithoutNetwork(t *testing.T) {
	configureSetupCancellation(false)
	payload := t.TempDir()
	data := []byte(testSHA256([]byte("artifact")) + "  artifact.bin\n")
	if err := os.WriteFile(filepath.Join(payload, "SHA256SUMS"), data, 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{portable: true, payloadDir: payload}
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		t.Fatal("portable manifest attempted a network request")
		return nil, nil
	})}
	sums, err := releaseSumsForConfig(cfg, client, defaultReleaseURL, testSHA256(data))
	if err != nil {
		t.Fatal(err)
	}
	if sums["artifact.bin"] != testSHA256([]byte("artifact")) {
		t.Fatal("portable manifest did not return the authenticated entry")
	}
	if _, err := releaseSumsForConfig(cfg, client, defaultReleaseURL, testSHA256([]byte("tampered"))); err == nil {
		t.Fatal("portable manifest was accepted with the wrong trust root")
	}
}

func TestCopyPortableArtifactPublishesAtomically(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	src := filepath.Join(dir, "payload")
	dest := filepath.Join(dir, "installed")
	data := []byte("verified portable artifact")
	if err := os.WriteFile(src, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := copyPortableArtifact(src, dest, testSHA256(data), nil); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(data) {
		t.Fatalf("installed data = %q", got)
	}
	if _, err := os.Stat(dest + ".part"); !os.IsNotExist(err) {
		t.Fatalf("staging file remains: %v", err)
	}
}

func TestCopyPortableArtifactRejectsBadChecksum(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	src := filepath.Join(dir, "payload")
	dest := filepath.Join(dir, "installed")
	if err := os.WriteFile(src, []byte("payload"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := copyPortableArtifact(src, dest, testSHA256([]byte("different")), nil); err == nil {
		t.Fatal("bad checksum was accepted")
	}
	for _, path := range []string{dest, dest + ".part"} {
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Fatalf("failed copy published %s: %v", path, err)
		}
	}
}

func TestCopyPortableArtifactHonorsCancellation(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	dir := t.TempDir()
	src := filepath.Join(dir, "payload")
	dest := filepath.Join(dir, "installed")
	data := []byte("payload")
	if err := os.WriteFile(src, data, 0o644); err != nil {
		t.Fatal(err)
	}
	requestSetupCancel()
	if err := copyPortableArtifact(src, dest, testSHA256(data), nil); err == nil {
		t.Fatal("cancelled copy succeeded")
	}
	for _, path := range []string{dest, dest + ".part"} {
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Fatalf("cancelled copy published %s: %v", path, err)
		}
	}
}
