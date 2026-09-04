package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDataLocationPointerRoundTrip(t *testing.T) {
	root := t.TempDir()
	defaultDir := filepath.Join(root, "local", defaultDataDirectoryName)
	selected := filepath.Join(root, "other drive", defaultDataDirectoryName)
	if err := saveDataLocationPointer(defaultDir, selected); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dataLocationPointerPath(defaultDir) + ".part"); !os.IsNotExist(err) {
		t.Fatal("staging file left behind")
	}
	got, ok, err := loadDataLocationPointer(defaultDir)
	if err != nil || !ok || got != filepath.Clean(selected) {
		t.Fatalf("pointer = %q, %v, %v", got, ok, err)
	}
}

func TestDataLocationPointerMissingIsDefault(t *testing.T) {
	if got, ok, err := loadDataLocationPointer(filepath.Join(t.TempDir(), "missing")); err != nil || ok || got != "" {
		t.Fatalf("missing pointer = %q, %v, %v", got, ok, err)
	}
}

func TestResolveStandardDataDirectoryPrecedence(t *testing.T) {
	root := t.TempDir()
	defaultDir := filepath.Join(root, "default")
	remembered := filepath.Join(root, "remembered")
	explicit := filepath.Join(root, "explicit")
	if err := saveDataLocationPointer(defaultDir, remembered); err != nil {
		t.Fatal(err)
	}
	called := false
	chooser := func(string) (string, bool, error) {
		called = true
		return "", false, nil
	}
	if got, proceed, err := resolveStandardDataDirectory(defaultDir, explicit, true, true, chooser); err != nil || !proceed || got != explicit {
		t.Fatalf("explicit directory = %q, %v, %v", got, proceed, err)
	}
	if called {
		t.Fatal("explicit directory opened the chooser")
	}
	if got, proceed, err := resolveStandardDataDirectory(defaultDir, defaultDir, false, true, chooser); err != nil || !proceed || got != remembered {
		t.Fatalf("remembered directory = %q, %v, %v", got, proceed, err)
	}
	if called {
		t.Fatal("remembered directory opened the chooser")
	}
}

func TestResolveStandardDataDirectoryKeepsExistingDefault(t *testing.T) {
	defaultDir := filepath.Join(t.TempDir(), "default")
	if err := os.MkdirAll(defaultDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(defaultDir, "partial-download"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	called := false
	chooser := func(string) (string, bool, error) {
		called = true
		return "", false, nil
	}
	if got, proceed, err := resolveStandardDataDirectory(defaultDir, defaultDir, false, true, chooser); err != nil || !proceed || got != defaultDir {
		t.Fatalf("existing default = %q, %v, %v", got, proceed, err)
	}
	if called {
		t.Fatal("existing default opened the chooser")
	}
}

func TestResolveStandardDataDirectoryPersistsFirstChoice(t *testing.T) {
	root := t.TempDir()
	defaultDir := filepath.Join(root, "default")
	selected := filepath.Join(root, "selected")
	chooser := func(gotDefault string) (string, bool, error) {
		if gotDefault != defaultDir {
			t.Fatalf("chooser default = %q, want %q", gotDefault, defaultDir)
		}
		return selected, true, nil
	}
	got, proceed, err := resolveStandardDataDirectory(defaultDir, defaultDir, false, true, chooser)
	if err != nil || !proceed || got != selected {
		t.Fatalf("first choice = %q, %v, %v", got, proceed, err)
	}
	remembered, found, err := loadDataLocationPointer(defaultDir)
	if err != nil || !found || remembered != selected {
		t.Fatalf("saved choice = %q, %v, %v", remembered, found, err)
	}
}

func TestResolveStandardDataDirectoryHonorsCancel(t *testing.T) {
	defaultDir := filepath.Join(t.TempDir(), "default")
	chooser := func(string) (string, bool, error) { return "", false, nil }
	if got, proceed, err := resolveStandardDataDirectory(defaultDir, defaultDir, false, true, chooser); err != nil || proceed || got != "" {
		t.Fatalf("cancel = %q, %v, %v", got, proceed, err)
	}
	if _, err := os.Stat(dataLocationPointerPath(defaultDir)); !os.IsNotExist(err) {
		t.Fatalf("cancel wrote a pointer: %v", err)
	}
}

func TestDataLocationPointerRejectsDamage(t *testing.T) {
	root := t.TempDir()
	defaultDir := filepath.Join(root, "default")
	if err := os.MkdirAll(defaultDir, 0o755); err != nil {
		t.Fatal(err)
	}
	cases := map[string]string{
		"garbage":  "{not json",
		"version":  fmt.Sprintf(`{"version":2,"path":%q}`, filepath.Join(root, "other")),
		"relative": `{"version":1,"path":"relative"}`,
		"unknown":  fmt.Sprintf(`{"version":1,"path":%q,"extra":true}`, filepath.Join(root, "other")),
		"trailing": fmt.Sprintf(`{"version":1,"path":%q} true`, filepath.Join(root, "other")),
		"default":  fmt.Sprintf(`{"version":1,"path":%q}`, defaultDir),
		"oversize": strings.Repeat(" ", maxDataLocationBytes+1),
	}
	for name, content := range cases {
		t.Run(name, func(t *testing.T) {
			if err := os.WriteFile(dataLocationPointerPath(defaultDir), []byte(content), 0o644); err != nil {
				t.Fatal(err)
			}
			if _, _, err := loadDataLocationPointer(defaultDir); err == nil {
				t.Fatal("damaged pointer accepted")
			}
		})
	}
}

func TestDataDirectoryForSelection(t *testing.T) {
	root := t.TempDir()
	want := filepath.Join(root, defaultDataDirectoryName)
	if got, err := dataDirectoryForSelection(root); err != nil || got != want {
		t.Fatalf("parent selection = %q, %v, want %q", got, err, want)
	}
	if got, err := dataDirectoryForSelection(want); err != nil || got != want {
		t.Fatalf("named selection = %q, %v, want %q", got, err, want)
	}
}

func TestDataDirectoryUnclaimed(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "data")
	if empty, err := dataDirectoryUnclaimed(dir); err != nil || !empty {
		t.Fatalf("missing directory = %v, %v", empty, err)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if empty, err := dataDirectoryUnclaimed(dir); err != nil || !empty {
		t.Fatalf("empty directory = %v, %v", empty, err)
	}
	if err := os.WriteFile(filepath.Join(dir, "partial"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if empty, err := dataDirectoryUnclaimed(dir); err != nil || empty {
		t.Fatalf("claimed directory = %v, %v", empty, err)
	}
}

func TestEnsureDataDirectoryWritable(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "new", "data")
	if err := ensureDataDirectoryWritable(dir); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 0 {
		t.Fatalf("write check left files behind: %v, %v", entries, err)
	}
}
