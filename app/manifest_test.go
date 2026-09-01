package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestDefaultReleaseManifestPin(t *testing.T) {
	// Read the embedded manifest rather than a hardcoded fixture path, so a
	// release pin does not have to remember to retag this test too.
	sums, err := parseVerifiedSums(defaultSums, defaultSumsSHA256)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{
		"build-spec.json",
		"guest-manifest.json",
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

func TestReleaseManifestRejectsDuplicateEntry(t *testing.T) {
	digest := strings.Repeat("a", 64)
	data := []byte(digest + "  payload.zip\n" + digest + "  payload.zip\n")
	manifestDigest := sha256.Sum256(data)
	if _, err := parseVerifiedSums(data, hex.EncodeToString(manifestDigest[:])); err == nil {
		t.Fatal("duplicate manifest entry was accepted")
	}
}

func TestReleaseManifestRejectsEmptyManifest(t *testing.T) {
	data := []byte("\n")
	digest := sha256.Sum256(data)
	if _, err := parseVerifiedSums(data, hex.EncodeToString(digest[:])); err == nil {
		t.Fatal("empty manifest was accepted")
	}
}

func TestReleaseManifestRejectsOversizedManifest(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(bytes.Repeat([]byte("x"), maxSumsBytes+1))
	}))
	defer server.Close()

	if _, err := fetchSums(server.Client(), server.URL, strings.Repeat("a", 64)); err == nil {
		t.Fatal("oversized manifest was accepted")
	}
}
