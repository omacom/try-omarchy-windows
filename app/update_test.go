package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func signedUpdateServer(t *testing.T, body []byte, mutateSignature bool) (*httptest.Server, ed25519.PublicKey) {
	t.Helper()
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	sig := ed25519.Sign(privateKey, body)
	if mutateSignature {
		sig[0] ^= 1
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/update.json":
			_, _ = w.Write(body)
		case "/update.json.sig":
			_, _ = fmt.Fprintln(w, base64.StdEncoding.EncodeToString(sig))
		default:
			http.NotFound(w, request)
		}
	}))
	return server, publicKey
}

func validUpdateJSON(version string) []byte {
	return []byte(fmt.Sprintf(`{"schema":1,"version":%q,"release":%q,"manifestSHA256":%q,"launcher":{"name":"TryOmarchy.exe","sha256":%q}}`,
		version,
		transferredReleaseBase+version,
		strings.Repeat("a", 64), strings.Repeat("b", 64)))
}

func TestFetchUpdateManifestAuthenticatesMetadata(t *testing.T) {
	server, key := signedUpdateServer(t, validUpdateJSON("v0.0.7-preview"), false)
	defer server.Close()
	manifest, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key)
	if err != nil {
		t.Fatal(err)
	}
	if manifest.Version != "v0.0.7-preview" {
		t.Fatalf("version = %q", manifest.Version)
	}
}

func TestFetchUpdateManifestRejectsBadSignature(t *testing.T) {
	server, key := signedUpdateServer(t, validUpdateJSON("v0.0.7-preview"), true)
	defer server.Close()
	if _, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key); err == nil {
		t.Fatal("tampered signature was accepted")
	}
}

func TestFetchUpdateManifestRejectsUnexpectedRelease(t *testing.T) {
	body := []byte(`{"schema":1,"version":"v0.0.7-preview","release":"https://example.test/release","manifestSHA256":"` + strings.Repeat("a", 64) + `","launcher":{"name":"TryOmarchy.exe","sha256":"` + strings.Repeat("b", 64) + `"}}`)
	server, key := signedUpdateServer(t, body, false)
	defer server.Close()
	if _, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key); err == nil {
		t.Fatal("unexpected release URL was accepted")
	}
}

func TestValidateUpdateManifestAcceptsOfficialRepository(t *testing.T) {
	var manifest updateManifest
	if err := json.Unmarshal(validUpdateJSON("v0.0.8-preview"), &manifest); err != nil {
		t.Fatal(err)
	}
	manifest.Release = officialReleaseBase + manifest.Version
	if err := validateUpdateManifest(&manifest); err != nil {
		t.Fatal(err)
	}
}

func TestValidateUpdateManifestAcceptsLegacyRepository(t *testing.T) {
	var manifest updateManifest
	if err := json.Unmarshal(validUpdateJSON("v0.0.8-preview"), &manifest); err != nil {
		t.Fatal(err)
	}
	manifest.Release = legacyReleaseBase + manifest.Version
	if err := validateUpdateManifest(&manifest); err != nil {
		t.Fatal(err)
	}
}

func TestUpdateVersionOrdering(t *testing.T) {
	for _, tc := range []struct {
		candidate string
		current   string
		newer     bool
	}{
		{"v0.0.7-preview", "v0.0.6-preview", true},
		{"v0.1.0-preview", "v0.0.99-preview", true},
		{"v1.0.0-preview", "v0.99.99-preview", true},
		{"v0.0.6-preview", "v0.0.6-preview", false},
		{"v0.0.5-preview", "v0.0.6-preview", false},
		{"latest", "v0.0.6-preview", false},
		{"v1.0.0", "v0.0.12-preview", true},
		{"v1.0.0", "v1.0.0-preview", true},
		{"v1.0.0", "v1.0.0", false},
		{"v1.0.1", "v1.0.0", true},
		{"v1.0.0-preview", "v1.0.0", false},
		{"v1.1.0-preview", "v1.0.0", false},
		{"v0.9.0", "v1.0.0-preview", false},
		{"v01.0.0", "v0.0.12-preview", false},
		{"v1.0.0-rc.1", "v0.0.12-preview", false},
		{"v1.0.0+build", "v0.0.12-preview", false},
		{"v999999999999999999999999.0.0", "v0.0.12-preview", false},
	} {
		if got := updateIsNewer(tc.candidate, tc.current); got != tc.newer {
			t.Errorf("updateIsNewer(%q, %q) = %v", tc.candidate, tc.current, got)
		}
	}
}

func TestEmbeddedUpdatePublicKey(t *testing.T) {
	key, err := updatePublicKey()
	if err != nil {
		t.Fatal(err)
	}
	if len(key) != ed25519.PublicKeySize {
		t.Fatalf("public key length = %d", len(key))
	}
}

func TestFetchStableUpdateAuthenticatesMetadata(t *testing.T) {
	server, key := signedUpdateServer(t, validUpdateJSON("v1.0.0"), false)
	defer server.Close()
	manifest, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key)
	if err != nil {
		t.Fatal(err)
	}
	if manifest.Version != "v1.0.0" {
		t.Fatalf("version = %q", manifest.Version)
	}
}

func TestMissedBridgeUsesSeparateSignedFeeds(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	bridge := validUpdateJSON("v0.0.12-preview")
	stable := validUpdateJSON("v1.0.0")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var data []byte
		switch strings.TrimSuffix(r.URL.Path, ".sig") {
		case "/update.json":
			data = bridge
		case "/update-v2.json":
			data = stable
		default:
			http.NotFound(w, r)
			return
		}
		if strings.HasSuffix(r.URL.Path, ".sig") {
			fmt.Fprintln(w, base64.StdEncoding.EncodeToString(ed25519.Sign(private, data)))
		} else {
			w.Write(data)
		}
	}))
	defer server.Close()
	oldFeed, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", public)
	if err != nil {
		t.Fatal(err)
	}
	if oldFeed.Version != "v0.0.12-preview" || !updateIsNewer(oldFeed.Version, "v0.0.11-preview") {
		t.Fatal("old launcher cannot reach the bridge preview")
	}
	newFeed, err := fetchUpdateManifest(server.Client(), server.URL+"/update-v2.json", public)
	if err != nil {
		t.Fatal(err)
	}
	if !updateIsNewer(newFeed.Version, oldFeed.Version) || newFeed.Version != "v1.0.0" {
		t.Fatal("bridge cannot reach stable")
	}
}
