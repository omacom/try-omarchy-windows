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
