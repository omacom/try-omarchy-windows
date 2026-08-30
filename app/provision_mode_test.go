package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestProvisionModeRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if _, ok := readProvisionMode(dir); ok {
		t.Fatal("empty install has a provisioning mode")
	}
	if err := writeProvisionMode(dir, provisionModeInstant); err != nil {
		t.Fatal(err)
	}
	if mode, ok := readProvisionMode(dir); !ok || mode != provisionModeInstant {
		t.Fatalf("mode = %q, %t", mode, ok)
	}
	if err := writeProvisionMode(dir, provisionModePersonal); err != nil {
		t.Fatal(err)
	}
	if mode, ok := readProvisionMode(dir); !ok || mode != provisionModePersonal {
		t.Fatalf("mode = %q, %t", mode, ok)
	}
}

func TestProvisionModeRejectsUnknownValues(t *testing.T) {
	dir := t.TempDir()
	if err := writeProvisionMode(dir, "surprise"); err == nil {
		t.Fatal("unknown mode was accepted")
	}
	if err := os.WriteFile(filepath.Join(dir, provisionModeFilename), []byte("corrupt\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, ok := readProvisionMode(dir); ok {
		t.Fatal("corrupt mode was accepted")
	}
}

func TestProvisionAccountHints(t *testing.T) {
	instant := provisionAccountHint(true)
	if instant != "Trial account: omarchy    Password: omarchy" {
		t.Fatalf("instant hint = %q", instant)
	}
	personal := provisionAccountHint(false)
	if personal == "" || personal == instant {
		t.Fatalf("personal hint = %q", personal)
	}
}
