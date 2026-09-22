//go:build windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestSettingsShortcutArguments(t *testing.T) {
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
	if got := settingsShortcutArguments(defaultDir); got != "-settings" {
		t.Fatalf("default settings shortcut arguments = %q", got)
	}
	custom := `D:\Try Omarchy Test`
	want := `-dir "D:\Try Omarchy Test" -settings`
	if got := settingsShortcutArguments(custom); got != want {
		t.Fatalf("custom settings shortcut arguments = %q, want %q", got, want)
	}
}

func TestLaunchShortcutArguments(t *testing.T) {
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
	if got := launchShortcutArguments(defaultDir, true); got != "-start" {
		t.Fatalf("default automatic shortcut arguments = %q", got)
	}
	custom := `D:\Try Omarchy Test`
	want := `-dir "D:\Try Omarchy Test" -start`
	if got := launchShortcutArguments(custom, true); got != want {
		t.Fatalf("custom automatic shortcut arguments = %q, want %q", got, want)
	}
	if got := launchShortcutArguments(custom, false); got != shortcutArguments(custom) {
		t.Fatalf("manual shortcut arguments = %q", got)
	}
}
