package main

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
)

const (
	currentVersion         = "v0.0.13-preview"
	defaultUpdateURL       = "https://github.com/omacom/try-omarchy-windows/releases/latest/download/update-v2.json"
	legacyReleaseBase      = "https://github.com/tsouth89/try-omarchy-windows/releases/download/"
	transferredReleaseBase = "https://github.com/omacom/try-omarchy-windows/releases/download/"
	officialReleaseBase    = "https://github.com/omacom/omarchy-win/releases/download/"
	maxUpdateManifestLen   = 64 << 10
	maxUpdateSignatureLen  = 4 << 10
	// Rotating the private half requires shipping a launcher that trusts both
	// the old and new keys before publishing manifests signed only by the new
	// key. The release workflow keeps the private key in the protected release
	// environment.
	updatePublicKeyHex = "f1edc8c2fc8fc8a7a108832eb93a9d9f2f8c07c5547fc4e4cb805c3b1615c9cd"
)

var releaseVersionPattern = regexp.MustCompile(`^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-preview)?$`)

type updateManifest struct {
	Schema         int    `json:"schema"`
	Version        string `json:"version"`
	Release        string `json:"release"`
	ManifestSHA256 string `json:"manifestSHA256"`
	Launcher       struct {
		Name   string `json:"name"`
		SHA256 string `json:"sha256"`
	} `json:"launcher"`
}

func fetchUpdateManifest(client *http.Client, manifestURL string, publicKey ed25519.PublicKey) (*updateManifest, error) {
	data, err := fetchSmallFile(client, manifestURL, maxUpdateManifestLen)
	if err != nil {
		return nil, fmt.Errorf("downloading update manifest: %w", err)
	}
	sigText, err := fetchSmallFile(client, manifestURL+".sig", maxUpdateSignatureLen)
	if err != nil {
		return nil, fmt.Errorf("downloading update signature: %w", err)
	}
	sig, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sigText)))
	if err != nil || len(sig) != ed25519.SignatureSize {
		return nil, fmt.Errorf("update signature is invalid")
	}
	if len(publicKey) != ed25519.PublicKeySize || !ed25519.Verify(publicKey, data, sig) {
		return nil, fmt.Errorf("update manifest authentication failed")
	}
	var manifest updateManifest
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&manifest); err != nil {
		return nil, fmt.Errorf("parsing update manifest: %w", err)
	}
	if dec.Decode(&struct{}{}) != io.EOF {
		return nil, fmt.Errorf("update manifest contains trailing data")
	}
	if err := validateUpdateManifest(&manifest); err != nil {
		return nil, err
	}
	return &manifest, nil
}

func fetchSmallFile(client *http.Client, source string, limit int64) ([]byte, error) {
	resp, err := getWithSetupRetry(client, source, 2)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("response exceeds %d bytes", limit)
	}
	return data, nil
}

func validateUpdateManifest(manifest *updateManifest) error {
	if manifest.Schema != 1 {
		return fmt.Errorf("unsupported update manifest schema %d", manifest.Schema)
	}
	if _, ok := parseReleaseVersion(manifest.Version); !ok {
		return fmt.Errorf("invalid update version %q", manifest.Version)
	}
	release := normalizedRelease(manifest.Release)
	if release != legacyReleaseBase+manifest.Version && release != transferredReleaseBase+manifest.Version &&
		release != officialReleaseBase+manifest.Version {
		return fmt.Errorf("update release URL does not match version")
	}
	if !validSHA256(manifest.ManifestSHA256) || !validSHA256(manifest.Launcher.SHA256) {
		return fmt.Errorf("update manifest contains an invalid SHA256")
	}
	if manifest.Launcher.Name != stableLauncherName {
		return fmt.Errorf("unexpected launcher name %q", manifest.Launcher.Name)
	}
	parsed, err := url.Parse(manifest.Release)
	if err != nil || parsed.Scheme != "https" || parsed.Host != "github.com" {
		return fmt.Errorf("invalid update release URL")
	}
	return nil
}

func updatePublicKey() (ed25519.PublicKey, error) {
	decoded, err := hex.DecodeString(updatePublicKeyHex)
	if err != nil || len(decoded) != ed25519.PublicKeySize {
		return nil, fmt.Errorf("embedded update key is invalid")
	}
	return ed25519.PublicKey(decoded), nil
}

func updateIsNewer(candidate, current string) bool {
	a, okA := parseReleaseVersion(candidate)
	b, okB := parseReleaseVersion(current)
	// Stable installations do not opt into preview updates.
	if !okA || !okB || (b[3] == 1 && a[3] == 0) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return a[i] > b[i]
		}
	}
	return false
}

func parseReleaseVersion(value string) ([4]int, bool) {
	var out [4]int
	match := releaseVersionPattern.FindStringSubmatch(value)
	if match == nil {
		return out, false
	}
	for i := 0; i < 3; i++ {
		n, err := strconv.Atoi(match[i+1])
		if err != nil {
			return out, false
		}
		out[i] = n
	}
	if match[4] == "" {
		out[3] = 1 // A stable release follows its preview at the same version.
	}
	return out, true
}
