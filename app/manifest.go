package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const maxSumsBytes = 1 << 20

// Variables so the signed test-launcher workflow can pin an isolated release
// with -ldflags -X. Normal builds retain these production defaults.
var (
	defaultReleaseURL        = "https://github.com/tsouth89/try-omarchy-windows/releases/download/v0.0.8-preview"
	defaultSumsSHA256        = "71603779581dd1b022dad43bf1826b26da737614e9f3e486524d92528ea7399d"
	defaultRuntimeReleaseURL = ""
	defaultRuntimeSumsSHA256 = ""
)

//go:embed testdata/SHA256SUMS.v0.0.8-preview
var defaultSums []byte

// releaseSums returns the embedded, authenticated manifest for the default
// release. Custom release URLs still fetch a manifest and authenticate it
// against the digest supplied by the caller.
func releaseSums(client *http.Client, release, expectedSHA256 string) (map[string]string, error) {
	embeddedDigest := sha256.Sum256(defaultSums)
	embeddedSHA256 := hex.EncodeToString(embeddedDigest[:])
	if normalizedRelease(release) == defaultReleaseURL &&
		normalizedSHA256(expectedSHA256) == defaultSumsSHA256 &&
		normalizedSHA256(defaultSumsSHA256) == embeddedSHA256 {
		return parseVerifiedSums(defaultSums, defaultSumsSHA256)
	}
	return fetchSums(client, normalizedRelease(release), expectedSHA256)
}

// fetchSums authenticates the release manifest against a digest embedded in
// the launcher. Fetching checksums beside the payload without an independent
// trust root would allow both to be replaced together.
func fetchSums(client *http.Client, release, expectedSHA256 string) (map[string]string, error) {
	if !validSHA256(normalizedSHA256(expectedSHA256)) {
		return nil, fmt.Errorf("trusted SHA256SUMS digest is not a valid SHA256")
	}
	resp, err := getWithSetupRetry(client, release+"/SHA256SUMS", 5)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, maxSumsBytes+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxSumsBytes {
		return nil, fmt.Errorf("SHA256SUMS exceeds %d bytes", maxSumsBytes)
	}
	return parseVerifiedSums(data, expectedSHA256)
}

func parseVerifiedSums(data []byte, expectedSHA256 string) (map[string]string, error) {
	expectedSHA256 = normalizedSHA256(expectedSHA256)
	if !validSHA256(expectedSHA256) {
		return nil, fmt.Errorf("trusted SHA256SUMS digest is not a valid SHA256")
	}
	actual := sha256.Sum256(data)
	if hex.EncodeToString(actual[:]) != expectedSHA256 {
		return nil, fmt.Errorf("SHA256SUMS authentication failed")
	}

	sums := map[string]string{}
	sc := bufio.NewScanner(bytes.NewReader(data))
	for line := 1; sc.Scan(); line++ {
		fields := strings.Fields(sc.Text())
		if len(fields) == 0 {
			continue
		}
		if len(fields) != 2 {
			return nil, fmt.Errorf("invalid SHA256SUMS line %d", line)
		}
		digest := strings.ToLower(fields[0])
		name := strings.TrimPrefix(fields[1], "*")
		if !validSHA256(digest) || name == "" {
			return nil, fmt.Errorf("invalid SHA256SUMS line %d", line)
		}
		if _, exists := sums[name]; exists {
			return nil, fmt.Errorf("duplicate SHA256SUMS entry for %s", name)
		}
		sums[name] = digest
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if len(sums) == 0 {
		return nil, fmt.Errorf("SHA256SUMS contains no entries")
	}
	return sums, nil
}

func validSHA256(value string) bool {
	if len(value) != sha256.Size*2 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}
