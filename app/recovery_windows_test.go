//go:build windows

package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestRestoredShortcutsTargetOnlyRestoredFolder(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "Restored guest with spaces")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := createRestoredLaunchers(dir); err != nil {
		t.Fatal(err)
	}
	const script = `$ErrorActionPreference='Stop'; $shell=New-Object -ComObject WScript.Shell; $links=@(foreach($name in @('Start Omarchy.lnk','Settings.lnk')){ $link=$shell.CreateShortcut((Join-Path $env:TRYOMARCHY_TEST_DIR $name)); [pscustomobject]@{Name=$name;Target=$link.TargetPath;Arguments=$link.Arguments;Directory=$link.WorkingDirectory} }); ConvertTo-Json -Compress -InputObject $links`

	cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", script)
	cmd.Env = append(os.Environ(), "TRYOMARCHY_TEST_DIR="+dir)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("reading shortcuts: %v: %s", err, strings.TrimSpace(string(out)))
	}
	var links []struct{ Name, Target, Arguments, Directory string }
	if err := json.Unmarshal(out, &links); err != nil {
		t.Fatalf("shortcut metadata: %v: %s", err, out)
	}
	if len(links) != 2 {
		t.Fatalf("shortcuts: %s", out)
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
