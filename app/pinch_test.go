package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"
)

func TestPinchRequiresOptInAndSupportingRuntime(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "provenance", "sources.lock.json"), []byte(`{"qemu":{"patches":[{"file":"patches/qemu/0014-forward-windows-pinch.patch"}]}}`), 0600); err != nil {
		t.Fatal(err)
	}
	cfg := config{qemu: filepath.Join(dir, "bin", "qemu.exe"), useGpu: true, displays: 1}
	if pinchEnabled(&cfg) {
		t.Fatal("default must not intercept touchpad input")
	}
	cfg.experimentalPinch = true
	if !pinchEnabled(&cfg) {
		t.Fatal("supporting opt-in runtime disabled")
	}
	if !slices.Contains(buildQemuArgs(&cfg, ""), "virtio-pinch-pci") {
		t.Fatal("dedicated device missing")
	}
	cfg.displays = 2
	if pinchEnabled(&cfg) {
		t.Fatal("multiple consoles not accepted yet")
	}
	cfg.displays = 1
	cfg.useGpu = false
	if pinchEnabled(&cfg) {
		t.Fatal("fallback must retain ordinary input")
	}
	cfg.useGpu = true
	cfg.qemu = filepath.Join(t.TempDir(), "bin", "qemu.exe")
	if pinchEnabled(&cfg) {
		t.Fatal("old runtime enabled")
	}
}

func TestPinchEnvironmentCannotEnableItself(t *testing.T) {
	env := []string{"KEEP=yes", "omarchy_experimental_pinch=1"}
	if got := pinchEnvironment(env, false); !slices.Equal(got, []string{"KEEP=yes"}) {
		t.Fatal(got)
	}
	if got := pinchEnvironment(env, true); !slices.Equal(got, []string{"KEEP=yes", "OMARCHY_EXPERIMENTAL_PINCH=1"}) {
		t.Fatal(got)
	}
}
