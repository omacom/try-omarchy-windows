package main

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestCopyLauncherPublishesCompleteCopy(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "download.exe")
	target := filepath.Join(dir, "install", stableLauncherName)
	if err := os.WriteFile(source, []byte("signed launcher bytes"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := copyLauncher(source, target, os.Rename); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "signed launcher bytes" {
		t.Fatalf("stable launcher = %q", got)
	}
	if _, err := os.Stat(target + ".part"); !os.IsNotExist(err) {
		t.Fatalf("staging file remains: %v", err)
	}
}

func TestCopyLauncherReplacesExistingCopy(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "download.exe")
	target := filepath.Join(dir, "install", stableLauncherName)
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(source, []byte("new signed launcher"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(target, []byte("old launcher"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := copyLauncher(source, target, os.Rename); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "new signed launcher" {
		t.Fatalf("stable launcher = %q", got)
	}
}

func TestCopyLauncherKeepsExistingTargetOnPublishFailure(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "download.exe")
	target := filepath.Join(dir, stableLauncherName)
	if err := os.WriteFile(source, []byte("new"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(target, []byte("old"), 0o755); err != nil {
		t.Fatal(err)
	}
	wantErr := errors.New("replace failed")
	err := copyLauncher(source, target, func(string, string) error { return wantErr })
	if !errors.Is(err, wantErr) {
		t.Fatalf("copy returned %v", err)
	}
	got, readErr := os.ReadFile(target)
	if readErr != nil {
		t.Fatal(readErr)
	}
	if string(got) != "old" {
		t.Fatalf("existing launcher changed to %q", got)
	}
	if _, statErr := os.Stat(target + ".part"); !os.IsNotExist(statErr) {
		t.Fatalf("staging file remains: %v", statErr)
	}
}

func TestCopyLauncherSkipsSameFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), stableLauncherName)
	if err := os.WriteFile(path, []byte("launcher"), 0o755); err != nil {
		t.Fatal(err)
	}
	called := false
	if err := copyLauncher(path, path, func(string, string) error {
		called = true
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	if called {
		t.Fatal("same launcher was replaced")
	}
}

func TestShortcutOfferMarker(t *testing.T) {
	dir := t.TempDir()
	if shortcutOfferRecorded(dir) {
		t.Fatal("offer unexpectedly recorded")
	}
	if err := recordShortcutOffer(dir); err != nil {
		t.Fatal(err)
	}
	if !shortcutOfferRecorded(dir) {
		t.Fatal("offer was not recorded")
	}
}
