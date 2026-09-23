//go:build windows

package main

import (
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
	"unsafe"
)

func TestRestoredShortcutsTargetOnlyRestoredFolder(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "Restored guest 世界 with spaces")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := saveLaunchPreferences(dir, launchPreferences{StartAutomatically: false, LaunchAtSignIn: true}); err != nil {
		t.Fatal(err)
	}
	if err := createRestoredLaunchers(dir); err != nil {
		t.Fatal(err)
	}
	if prefs, err := loadLaunchPreferences(dir); err != nil || prefs.LaunchAtSignIn {
		t.Fatalf("restored sign-in preference = %#v, error %v", prefs, err)
	}
	var links []struct{ Name, Target, Arguments, Directory string }
	for _, name := range []string{"Start Omarchy.lnk", "Settings.lnk"} {
		target, arguments, directory := readShellLinkForTest(t, filepath.Join(dir, name))
		links = append(links, struct{ Name, Target, Arguments, Directory string }{name, target, arguments, directory})
	}
	if len(links) != 2 {
		t.Fatalf("shortcuts: %v", links)
	}
	for _, link := range links {
		// Windows Shell can expand 8.3 paths from the runner's temporary directory.
		// Compare file identity instead of treating equivalent paths as different.
		for _, pair := range [][2]string{{link.Target, filepath.Join(dir, stableLauncherName)}, {link.Directory, dir}} {
			actual, expected := pair[0], pair[1]
			got, err := os.Stat(actual)
			if err != nil {
				t.Fatalf("shortcut %s: %v", actual, err)
			}
			want, err := os.Stat(expected)
			if err != nil {
				t.Fatal(err)
			}
			if !os.SameFile(got, want) {
				t.Fatalf("shortcut %s points to %s, expected %s", link.Name, actual, expected)
			}
		}
		wantArgs := `-dir "` + dir + `"`
		if link.Name == "Settings.lnk" {
			wantArgs += " -settings"
		} else if link.Name != "Start Omarchy.lnk" {
			t.Fatalf("unexpected shortcut %q", link.Name)
		}
		if link.Arguments != wantArgs {
			t.Fatalf("shortcut arguments = %q, want %q", link.Arguments, wantArgs)
		}
	}

	if !shortcutOfferRecorded(dir) {
		t.Fatal("restored launch could offer to replace original shortcuts")
	}
}

// Read through IShellLinkW as well: WScript's getter can return an empty target
// for a valid Unicode link, even when Explorer can open it.
func readShellLinkForTest(t *testing.T, path string) (string, string, string) {
	t.Helper()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	hr, _, _ := ole32.NewProc("CoInitializeEx").Call(0, 2)
	if int32(hr) < 0 {
		t.Fatalf("COM init: %x", hr)
	}
	defer ole32.NewProc("CoUninitialize").Call()
	class := recoveryGUID{0x00021401, 0, 0, [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	iid := recoveryGUID{0x000214f9, 0, 0, [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	var link uintptr
	hr, _, _ = ole32.NewProc("CoCreateInstance").Call(uintptr(unsafe.Pointer(&class)), 0, 1, uintptr(unsafe.Pointer(&iid)), uintptr(unsafe.Pointer(&link)))
	if int32(hr) < 0 {
		t.Fatalf("create link: %x", hr)
	}
	defer recoveryCOMCall(link, 2)
	persistIID := recoveryGUID{0x0000010b, 0, 0, [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	var persist uintptr
	if hr := recoveryCOMCall(link, 0, uintptr(unsafe.Pointer(&persistIID)), uintptr(unsafe.Pointer(&persist))); int32(hr) < 0 {
		t.Fatalf("IPersistFile: %x", hr)
	}
	defer recoveryCOMCall(persist, 2)
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		t.Fatal(err)
	}
	if hr := recoveryCOMCall(persist, 5, uintptr(unsafe.Pointer(name)), 0); int32(hr) < 0 {
		t.Fatalf("load link: %x", hr)
	}
	values := make([]string, 3)
	for i, method := range []int{3, 10, 8} {
		buffer := make([]uint16, 32768)
		args := []uintptr{uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer))}
		if method == 3 {
			args = append(args, 0, 4)
		} // SLGP_RAWPATH
		if hr := recoveryCOMCall(link, method, args...); int32(hr) < 0 {
			t.Fatalf("read link property: %x", hr)
		}
		values[i] = syscall.UTF16ToString(buffer)
	}
	return values[0], values[1], values[2]
}

func TestUnicodeShortcutOwnership(t *testing.T) {
	dir := t.TempDir()
	old := filepath.Join(dir, "原本", "TryOmarchy.exe")
	next := filepath.Join(dir, "Moved 世界", "TryOmarchy.exe")
	foreign := filepath.Join(dir, "Other", "TryOmarchy.exe")
	paths := []string{filepath.Join(dir, "owned.lnk"), filepath.Join(dir, "foreign.lnk")}
	for i, target := range []string{old, foreign} {
		if err := os.MkdirAll(filepath.Dir(target), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(target, []byte("fixture"), 0600); err != nil {
			t.Fatal(err)
		}
		if err := writeShellLink(paths[i], target, "-settings", filepath.Dir(target)); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.MkdirAll(filepath.Dir(next), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(next, []byte("fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(paths[1])
	if err != nil {
		t.Fatal(err)
	}
	initialTarget, initialArgs, initialErr := readShellLink(paths[0])
	changed := false
	if err := changeOwnedShortcuts(paths, []string{old, next}, func(path, args string) error {
		changed = true
		return writeShellLink(path, next, args, filepath.Dir(next))
	}); err != nil {
		t.Fatal(err)
	}
	target, args, err := readShellLink(paths[0])
	if err != nil {
		t.Fatal(err)
	}
	if !sameShortcutTarget(target, next) || args != "-settings" {
		t.Fatalf("moved link = %q %q; initial=%q %q err=%v; expected old=%q; callback=%v", target, args, initialTarget, initialArgs, initialErr, old, changed)
	}
	after, err := os.ReadFile(paths[1])
	if err != nil {
		t.Fatal(err)
	}
	if string(before) != string(after) {
		t.Fatal("another installation's shortcut changed")
	}
	if err := changeOwnedShortcuts(paths, []string{next}, func(path, _ string) error { return os.Remove(path) }); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(paths[0]); !os.IsNotExist(err) {
		t.Fatal("owned shortcut remains")
	}
	if _, err := os.Stat(paths[1]); err != nil {
		t.Fatal("foreign shortcut removed", err)
	}
}

func TestShortcutOwnershipAcceptsFileAliases(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "TryOmarchy.exe")
	alias := filepath.Join(dir, "alias.exe")
	foreign := filepath.Join(dir, "foreign.exe")
	for _, path := range []string{target, foreign} {
		if err := os.WriteFile(path, []byte("same bytes, different identity"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Link(target, alias); err != nil {
		t.Fatal(err)
	}
	if !sameShortcutTarget(alias, target) {
		t.Fatal("file alias was not recognized")
	}
	if sameShortcutTarget(foreign, target) {
		t.Fatal("different executable was treated as owned")
	}
	if !sameShortcutTarget(filepath.Join(dir, "missing.exe"), filepath.Join(dir, "missing.exe")) {
		t.Fatal("literal ownership of a removed target was lost")
	}
}
