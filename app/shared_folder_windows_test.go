//go:build windows

package main

import (
	"os"
	"path/filepath"
	"testing"
	"unsafe"
)

func TestValidateWindowsSharedFolderPolicy(t *testing.T) {
	for _, name := range []string{"SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData", "PUBLIC", "LOCALAPPDATA", "APPDATA"} {
		t.Setenv(name, "")
	}
	root := t.TempDir()
	home := filepath.Join(root, "home")
	share := filepath.Join(home, "Omarchy Shared")
	data := filepath.Join(home, "TryOmarchyData")
	for _, path := range []string{share, data} {
		if err := os.MkdirAll(path, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	got, err := validateWindowsSharedFolder(share, data, home)
	if err != nil {
		t.Fatalf("valid share = %q, %v", got, err)
	}
	want, err := filepath.EvalSymlinks(share)
	if err != nil {
		t.Fatal(err)
	}
	want, err = filepath.Abs(want)
	if err != nil {
		t.Fatal(err)
	}
	if !sameWindowsPath(got, want) {
		t.Fatalf("canonical share = %q, want %q", got, want)
	}
	if _, err := validateWindowsSharedFolder(home, data, home); err == nil {
		t.Fatal("the whole home folder was accepted")
	}
	if _, err := validateWindowsSharedFolder(root, data, home); err == nil {
		t.Fatal("an ancestor of the home folder was accepted")
	}
	if _, err := validateWindowsSharedFolder(data, data, home); err == nil {
		t.Fatal("the VM data directory was accepted")
	}
	if _, err := validateWindowsSharedFolder(home, share, root); err == nil {
		t.Fatal("a folder containing the VM data directory was accepted")
	}
	if _, err := validateWindowsSharedFolder(`\\server\share`, data, home); err == nil {
		t.Fatal("a UNC path was accepted")
	}
	protected := filepath.Join(root, "Windows")
	protectedChild := filepath.Join(protected, "Temp")
	if err := os.MkdirAll(protectedChild, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("SystemRoot", protected)
	if _, err := validateWindowsSharedFolder(protectedChild, data, home); err == nil {
		t.Fatal("a Windows system folder was accepted")
	}
}

func TestNotifyIconDataUsesCurrentWindowsLayout(t *testing.T) {
	if size := unsafe.Sizeof(notifyIconData{}); size != 976 {
		t.Fatalf("NOTIFYICONDATAW size = %d, want 976", size)
	}
}
