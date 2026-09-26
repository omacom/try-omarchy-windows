package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

func pinchRuntime(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "provenance", "sources.lock.json"), []byte(`{"qemu":{"patches":[{"file":"patches/qemu/0014-forward-windows-pinch.patch"}]}}`), 0600); err != nil {
		t.Fatal(err)
	}
	return filepath.Join(dir, "bin", "qemu.exe")
}

func TestPinchFollowsGuestDeclarationAndSupportingRuntime(t *testing.T) {
	cfg := config{qemu: pinchRuntime(t), useGpu: true, displays: 1}
	if pinchEnabled(&cfg) {
		t.Fatal("guest without the declaration received the touchpad")
	}
	cfg.guestPinch = true
	if !pinchEnabled(&cfg) {
		t.Fatal("declaring guest with a supporting runtime left pinch off")
	}
	if !slices.Contains(buildQemuArgs(&cfg, ""), "virtio-pinch-pci") {
		t.Fatal("dedicated device missing")
	}
	cfg.disablePinch = true
	if pinchEnabled(&cfg) || slices.Contains(buildQemuArgs(&cfg, ""), "virtio-pinch-pci") {
		t.Fatal("-disable-pinch ignored")
	}
	cfg.experimentalPinch = true
	if pinchEnabled(&cfg) {
		t.Fatal("-disable-pinch must win over -experimental-pinch")
	}
	cfg.disablePinch = false
	cfg.guestPinch = false
	if !pinchEnabled(&cfg) {
		t.Fatal("-experimental-pinch no longer forces a hand-configured guest")
	}
	cfg.guestPinch = true
	cfg.experimentalPinch = false
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

func TestGuestAcceptsPinchOnlyWhenDeclared(t *testing.T) {
	for _, tc := range []struct {
		spec string
		want bool
	}{
		{`{"runtime":{"kernelCommandLine":"root=/dev/vda"}}`, false},
		{`{"runtime":{"optionalDevices":[]}}`, false},
		{`{"runtime":{"optionalDevices":["virtio-pinch"]}}`, false},
		{`{"runtime":{"devices":["virtio-pinch-pci"]}}`, false},
		{`{"runtime":{"optionalDevices":["virtio-pinch-pci"]}}`, true},
	} {
		var spec buildSpec
		if err := json.Unmarshal([]byte(tc.spec), &spec); err != nil {
			t.Fatal(err)
		}
		if got := guestAcceptsPinch(spec); got != tc.want {
			t.Errorf("%s: got %v, want %v", tc.spec, got, tc.want)
		}
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
