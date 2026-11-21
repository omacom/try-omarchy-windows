package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestRuntimeHoldsWindows(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "bin", "qemu-system-x86_64w.exe")
	if runtimeHoldsWindows(exe) {
		t.Fatal("a runtime without provenance holds its windows")
	}
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0o755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "provenance", "sources.lock.json")
	for _, tc := range []struct {
		data  string
		holds bool
	}{
		{`{"qemu":{"patches":[{"file":"` + externalKeyboardHookPatch + `"}]}}`, false},
		{`{"qemu":{"patches":[{"file":"` + externalKeyboardHookPatch + `"},{"file":"` + heldWindowsPatch + `"}]}}`, true},
	} {
		if err := os.WriteFile(path, []byte(tc.data), 0o600); err != nil {
			t.Fatal(err)
		}
		if got := runtimeHoldsWindows(exe); got != tc.holds {
			t.Fatalf("holds = %v, want %v for %s", got, tc.holds, tc.data)
		}
	}
}

func TestRuntimeShowsHeldWindowsWithFrame(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "bin", "qemu-system-x86_64w.exe")
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0o755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "provenance", "sources.lock.json")
	for _, tc := range []struct {
		data string
		want bool
	}{
		{`{"qemu":{"patches":[{"file":"` + heldWindowsPatch + `"}]}}`, false},
		{`{"qemu":{"patches":[{"file":"` + heldWindowsPatch + `"},{"file":"` + heldWindowFramePatch + `"}]}}`, true},
	} {
		if err := os.WriteFile(path, []byte(tc.data), 0o600); err != nil {
			t.Fatal(err)
		}
		if got := runtimeShowsHeldWindowsWithFrame(exe); got != tc.want {
			t.Fatalf("shows with frame = %v, want %v for %s", got, tc.want, tc.data)
		}
	}
}

func TestHeldWindowsEnvironment(t *testing.T) {
	// A value left in the launcher's own environment never reaches QEMU, so a
	// runtime is told to hold its windows only when the curtain will show them.
	env := []string{"Path=example", heldWindowsEnv + "=5", "omarchy_sdl_hold_windows_ms=1"}
	if got, want := heldWindowsEnvironment(env, false), []string{"Path=example"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("off: got %v, want %v", got, want)
	}
	if got, want := heldWindowsEnvironment(env, true), []string{"Path=example", heldWindowsEnv + "=10000"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("on: got %v, want %v", got, want)
	}
}
