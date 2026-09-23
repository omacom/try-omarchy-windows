//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
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

func TestNewInstallPreservesForeignShortcuts(t *testing.T) {
	dir := t.TempDir()
	paths := []string{
		filepath.Join(dir, "Try Omarchy.lnk"),
		filepath.Join(dir, "Try Omarchy Settings.lnk"),
		filepath.Join(dir, "Desktop Try Omarchy.lnk"),
	}
	oldTarget := filepath.Join(dir, "old-install", stableLauncherName)
	newTarget := filepath.Join(dir, "new-install", stableLauncherName)
	if err := writeShellLink(paths[0], oldTarget, "-dir old", dir); err != nil {
		t.Fatal(err)
	}
	if err := writeLauncherShortcuts(paths, newTarget, filepath.Dir(newTarget), true, false, false); err == nil || !strings.Contains(err.Error(), "another installation") {
		t.Fatalf("expected shortcut conflict, got %v", err)
	}
	target, args, err := readShellLink(paths[0])
	if err != nil || !sameShortcutTarget(target, oldTarget) || args != "-dir old" {
		t.Fatalf("foreign shortcut changed: target=%q args=%q err=%v", target, args, err)
	}
	if _, err := os.Lstat(paths[1]); !os.IsNotExist(err) {
		t.Fatalf("settings shortcut was partially created: %v", err)
	}
}

func TestSecondInstallGetsFolderShortcutsAfterGlobalConflict(t *testing.T) {
	root := t.TempDir()
	oldTarget := filepath.Join(root, "old-install", stableLauncherName)
	dir := filepath.Join(root, "new-install")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	newTarget := filepath.Join(dir, stableLauncherName)
	paths := []string{
		filepath.Join(root, "Try Omarchy.lnk"),
		filepath.Join(root, "Try Omarchy Settings.lnk"),
		filepath.Join(root, "Desktop Try Omarchy.lnk"),
	}
	if err := writeShellLink(paths[0], oldTarget, "-dir old", root); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(paths[0])
	if err != nil {
		t.Fatal(err)
	}
	message, err := finishLauncherShortcutChoice(paths, newTarget, dir, true, false, true)
	if err != nil || !strings.Contains(message, "beside this installation") {
		t.Fatalf("folder fallback: message=%q err=%v", message, err)
	}
	if !shortcutOfferRecorded(dir) {
		t.Fatal("choice was not recorded; the prompt would return on the next launch")
	}
	after, err := os.ReadFile(paths[0])
	if err != nil || string(after) != string(before) {
		t.Fatalf("foreign Start Menu shortcut changed: %v", err)
	}
	for _, item := range []struct{ name, arguments string }{
		{"Start Omarchy.lnk", launchShortcutArguments(dir, true)},
		{"Settings.lnk", settingsShortcutArguments(dir)},
	} {
		target, arguments, err := readShellLink(filepath.Join(dir, item.name))
		if err != nil || !sameShortcutTarget(target, newTarget) || arguments != item.arguments {
			t.Fatalf("folder shortcut %s: target=%q arguments=%q err=%v", item.name, target, arguments, err)
		}
	}
}

func TestNewInstallRefreshesItsOwnShortcuts(t *testing.T) {
	dir := t.TempDir()
	paths := []string{
		filepath.Join(dir, "Try Omarchy.lnk"),
		filepath.Join(dir, "Try Omarchy Settings.lnk"),
		filepath.Join(dir, "Desktop Try Omarchy.lnk"),
	}
	target := filepath.Join(dir, stableLauncherName)
	if err := writeShellLink(paths[0], target, "-old", dir); err != nil {
		t.Fatal(err)
	}
	if err := writeLauncherShortcuts(paths, target, dir, true, true, true); err != nil {
		t.Fatal(err)
	}
	for i, path := range paths {
		gotTarget, args, err := readShellLink(path)
		if err != nil || !sameShortcutTarget(gotTarget, target) {
			t.Fatalf("shortcut %d: target=%q err=%v", i, gotTarget, err)
		}
		want := launchShortcutArguments(dir, true)
		if i == 1 {
			want = settingsShortcutArguments(dir)
		}
		if args != want {
			t.Fatalf("shortcut %d args=%q want %q", i, args, want)
		}
	}
}
