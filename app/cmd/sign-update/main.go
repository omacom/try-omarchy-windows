package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const expectedPublicKeyHex = "f1edc8c2fc8fc8a7a108832eb93a9d9f2f8c07c5547fc4e4cb805c3b1615c9cd"

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

func main() {
	version := flag.String("version", "", "release tag")
	release := flag.String("release", "", "release asset base URL")
	manifestSHA := flag.String("manifest-sha256", "", "authenticated SHA256SUMS digest")
	launcher := flag.String("launcher", "", "signed launcher path")
	output := flag.String("output", "update.json", "manifest output path")
	flag.Parse()
	if *version == "" || *release == "" || *launcher == "" || !validSHA256(*manifestSHA) {
		fatalf("version, release, launcher, and a valid manifest-sha256 are required")
	}
	launcherData, err := os.ReadFile(*launcher)
	if err != nil {
		fatalf("read launcher: %v", err)
	}
	launcherSum := sha256.Sum256(launcherData)
	manifest := updateManifest{
		Schema: 1, Version: *version, Release: strings.TrimRight(*release, "/"),
		ManifestSHA256: strings.ToLower(*manifestSHA),
	}
	manifest.Launcher.Name = "TryOmarchy.exe"
	manifest.Launcher.SHA256 = hex.EncodeToString(launcherSum[:])
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		fatalf("encode manifest: %v", err)
	}
	data = append(data, '\n')
	privateKey, err := signingKey()
	if err != nil {
		fatalf("load signing key: %v", err)
	}
	signature := ed25519.Sign(privateKey, data)
	if err := os.MkdirAll(filepath.Dir(*output), 0o755); err != nil && filepath.Dir(*output) != "." {
		fatalf("create output directory: %v", err)
	}
	if err := os.WriteFile(*output, data, 0o644); err != nil {
		fatalf("write manifest: %v", err)
	}
	sigText := base64.StdEncoding.EncodeToString(signature) + "\n"
	if err := os.WriteFile(*output+".sig", []byte(sigText), 0o644); err != nil {
		fatalf("write signature: %v", err)
	}
}

func signingKey() (ed25519.PrivateKey, error) {
	encoded := strings.TrimSpace(os.Getenv("TRYOMARCHY_UPDATE_SIGNING_KEY"))
	der, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, err
	}
	parsed, err := x509.ParsePKCS8PrivateKey(der)
	if err != nil {
		return nil, err
	}
	key, ok := parsed.(ed25519.PrivateKey)
	if !ok {
		return nil, fmt.Errorf("key is not Ed25519")
	}
	wantPublic, err := hex.DecodeString(expectedPublicKeyHex)
	if err != nil || !bytes.Equal(key.Public().(ed25519.PublicKey), wantPublic) {
		return nil, fmt.Errorf("key does not match the launcher's update trust root")
	}
	return key, nil
}

func validSHA256(value string) bool {
	if len(value) != sha256.Size*2 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func fatalf(format string, values ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", values...)
	os.Exit(1)
}
