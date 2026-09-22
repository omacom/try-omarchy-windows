//go:build windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestPlatformQMPControlDirectoryUsesWindowsTemp(t *testing.T) {
	base := t.TempDir()
	t.Setenv("TEMP", base)
	t.Setenv("TMP", base)

	got, err := platformQMPControlDirectory()
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(got) != "TryOmarchyIPC" {
		t.Fatalf("control directory = %q", got)
	}
	baseInfo, err := os.Stat(base)
	if err != nil {
		t.Fatal(err)
	}
	parentInfo, err := os.Stat(filepath.Dir(got))
	if err != nil {
		t.Fatal(err)
	}
	if !os.SameFile(baseInfo, parentInfo) {
		t.Fatalf("control directory parent = %q, want Windows temp %q", filepath.Dir(got), base)
	}
}
