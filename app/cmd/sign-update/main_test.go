package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
)

func TestVerifyBridge(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	const version = "v0.0.12-preview"
	base := updateManifest{Schema: 1, Version: version,
		Release:        "https://github.com/tsouth89/try-omarchy-windows/releases/download/" + version,
		ManifestSHA256: strings.Repeat("a", 64)}
	base.Launcher.Name = "TryOmarchy.exe"
	base.Launcher.SHA256 = strings.Repeat("b", 64)
	for _, tc := range []struct {
		name         string
		mutate       func(*updateManifest)
		badSignature bool
		wantErr      bool
	}{
		{name: "valid"},
		{name: "wrong version", mutate: func(m *updateManifest) { m.Version = "v0.0.11-preview" }, wantErr: true},
		{name: "unexpected URL", mutate: func(m *updateManifest) { m.Release = "https://example.com/" + version }, wantErr: true},
		{name: "bad hash", mutate: func(m *updateManifest) { m.Launcher.SHA256 = "bad" }, wantErr: true},
		{name: "wrong executable", mutate: func(m *updateManifest) { m.Launcher.Name = "other.exe" }, wantErr: true},
		{name: "wrong schema", mutate: func(m *updateManifest) { m.Schema = 2 }, wantErr: true},
		{name: "invalid signature", badSignature: true, wantErr: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			manifest := base
			if tc.mutate != nil {
				tc.mutate(&manifest)
			}
			data, err := json.Marshal(manifest)
			if err != nil {
				t.Fatal(err)
			}
			sig := ed25519.Sign(private, data)
			if tc.badSignature {
				sig[0] ^= 1
			}
			encoded := []byte(base64.StdEncoding.EncodeToString(sig))
			if err := verifyBridge(data, encoded, version, public); (err != nil) != tc.wantErr {
				t.Fatalf("verifyBridge error = %v, want error %v", err, tc.wantErr)
			}
			if err := verifyBridge(data, encoded, "v1.0.0", public); err == nil {
				t.Fatal("stable release accepted as a legacy bridge")
			}
		})
	}
}
