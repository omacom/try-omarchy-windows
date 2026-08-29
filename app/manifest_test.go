package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"
)

func TestDefaultReleaseManifestPin(t *testing.T) {
	data, err := os.ReadFile("testdata/SHA256SUMS.v0.0.3-preview")
	if err != nil {
		t.Fatal(err)
	}
	sums, err := parseVerifiedSums(data, defaultSumsSHA256)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{
		"build-spec.json",
		"initramfs-linux.img",
		"rootfs.ext4",
		"rootfs.ext4.zst",
		"vmlinuz-linux",
		"winq-emu-alpha10-portable.zip",
	} {
		if !validSHA256(sums[name]) {
			t.Errorf("missing valid checksum for %s", name)
		}
	}
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return f(req)
}

func TestDefaultReleaseUsesEmbeddedManifest(t *testing.T) {
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		return nil, fmt.Errorf("unexpected network request")
	})}
	sums, err := releaseSums(client, defaultReleaseURL, defaultSumsSHA256)
	if err != nil {
		t.Fatal(err)
	}
	if !validSHA256(sums["rootfs.ext4"]) {
		t.Fatal("embedded manifest is missing rootfs.ext4")
	}
}

func TestReleaseManifestRejectsTampering(t *testing.T) {
	data := []byte(strings.Repeat("a", 64) + "  payload.zip\n")
	digest := sha256.Sum256(data)
	data[len(data)-2] ^= 1
	if _, err := parseVerifiedSums(data, hex.EncodeToString(digest[:])); err == nil {
		t.Fatal("tampered manifest was accepted")
	}
}

func TestReleaseManifestRejectsInvalidTrustRoot(t *testing.T) {
	if _, err := parseVerifiedSums([]byte("data"), "not-a-sha256"); err == nil {
		t.Fatal("invalid trust root was accepted")
	}
}

func TestReleaseManifestRejectsMalformedEntry(t *testing.T) {
	data := []byte("not-a-checksum  payload.zip\n")
	digest := sha256.Sum256(data)
	if _, err := parseVerifiedSums(data, hex.EncodeToString(digest[:])); err == nil {
		t.Fatal("malformed manifest entry was accepted")
	}
}
