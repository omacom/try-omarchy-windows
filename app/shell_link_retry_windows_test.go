//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func shortcutRetryFixture(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	target := filepath.Join(dir, "Launcher 世界.exe")
	if err := os.WriteFile(target, []byte("fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "owned.lnk")
	if err := writeShellLink(link, target, "-settings", dir); err != nil {
		t.Fatal(err)
	}
	return link, target
}

func TestShortcutDeleteRetriesRealWindowsSharingLock(t *testing.T) {
	link, target := shortcutRetryFixture(t)
	name, _ := syscall.UTF16PtrFromString(link)
	handle, err := syscall.CreateFile(name, syscall.GENERIC_READ, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE, nil, syscall.OPEN_EXISTING, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if handle != syscall.InvalidHandle {
			syscall.CloseHandle(handle)
		}
	}()
	pauses := 0
	err = changeOwnedShortcutsWithPause([]string{link}, []string{target}, func(path, _ string) error { return os.Remove(path) }, func(time.Duration) {
		pauses++
		if handle != syscall.InvalidHandle {
			syscall.CloseHandle(handle)
			handle = syscall.InvalidHandle
		}
	})
	if err != nil || pauses == 0 {
		t.Fatalf("sharing retry: pauses=%d err=%v", pauses, err)
	}
	if _, err := os.Stat(link); !os.IsNotExist(err) {
		t.Fatal("shortcut was not removed", err)
	}
}

func TestShortcutRetryRechecksOwnership(t *testing.T) {
	link, target := shortcutRetryFixture(t)
	other := filepath.Join(filepath.Dir(target), "Another installation.exe")
	calls := 0
	err := changeOwnedShortcutsWithPause([]string{link}, []string{target}, func(string, string) error {
		calls++
		return syscall.Errno(32)
	}, func(time.Duration) {
		if err := writeShellLink(link, other, "-launcher", filepath.Dir(other)); err != nil {
			t.Fatal(err)
		}
	})
	if err != nil || calls != 1 {
		t.Fatalf("changed foreign shortcut: calls=%d err=%v", calls, err)
	}
	actual, _, err := readShellLink(link)
	if err != nil || !sameShortcutTarget(actual, other) {
		t.Fatalf("foreign shortcut lost: %q %v", actual, err)
	}
}

func TestShortcutRetryIsBoundedAndSpecific(t *testing.T) {
	for _, failure := range []syscall.Errno{32, 33, 5} {
		link, target := shortcutRetryFixture(t)
		calls, pauses := 0, 0
		err := changeOwnedShortcutsWithPause([]string{link}, []string{target}, func(path, _ string) error {
			calls++
			return &os.PathError{Op: "remove", Path: path, Err: failure}
		}, func(time.Duration) { pauses++ })
		want := 10
		if failure == 5 {
			want = 1
		}
		if !errors.Is(err, failure) || calls != want || pauses != want-1 {
			t.Fatalf("errno=%d calls=%d pauses=%d err=%v", failure, calls, pauses, err)
		}
	}
}
