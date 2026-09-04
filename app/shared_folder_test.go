package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestCreateRecommendedSharedFolder(t *testing.T) {
	home := t.TempDir()
	want := filepath.Join(home, recommendedSharedFolderName)
	got, err := createRecommendedSharedFolder(home)
	if err != nil || got != want {
		t.Fatalf("created path = %q, %v; want %q", got, err, want)
	}
	if info, err := os.Stat(got); err != nil || !info.IsDir() {
		t.Fatalf("recommended share was not a directory: %v, %v", info, err)
	}
	if second, err := createRecommendedSharedFolder(home); err != nil || second != want {
		t.Fatalf("existing share = %q, %v", second, err)
	}
}

func TestCreateRecommendedSharedFolderRejectsOccupiedPath(t *testing.T) {
	home := t.TempDir()
	path := filepath.Join(home, recommendedSharedFolderName)
	if err := os.WriteFile(path, []byte("mine"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := createRecommendedSharedFolder(home); err == nil {
		t.Fatal("a file was accepted as the recommended shared folder")
	}
}
