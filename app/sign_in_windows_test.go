//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSignInShortcutOwnership(t *testing.T) {
	target, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	link := filepath.Join(dir, "startup.lnk")
	if err := syncSignInShortcutAt(link, target, dir, true); err != nil {
		t.Fatal(err)
	}
	gotTarget, args, err := readShellLink(link)
	if err != nil || !sameShortcutTarget(gotTarget, target) || !strings.Contains(args, "-start") {
		t.Fatalf("startup link = %q %q, error %v", gotTarget, args, err)
	}
	foreign := filepath.Join(dir, "another.exe")
	if err := os.WriteFile(foreign, []byte("test"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writeShellLink(link, foreign, "", dir); err != nil {
		t.Fatal(err)
	}
	if err := syncSignInShortcutAt(link, target, dir, false); err == nil {
		t.Fatal("removed another installation's startup link")
	}
	if _, err := os.Stat(link); err != nil {
		t.Fatal(err)
	}
	if err := writeShellLink(link, target, "-start", dir); err != nil {
		t.Fatal(err)
	}
	if err := syncSignInShortcutAt(link, target, dir, false); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(link); !os.IsNotExist(err) {
		t.Fatalf("startup link remains: %v", err)
	}
}
