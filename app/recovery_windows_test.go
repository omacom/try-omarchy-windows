//go:build windows

package main

import (
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
	const script = `$ErrorActionPreference='Stop'; $shell=New-Object -ComObject WScript.Shell; foreach($name in @('Start Omarchy.lnk','Settings.lnk')){ $link=$shell.CreateShortcut((Join-Path $env:TRYOMARCHY_TEST_DIR $name)); if($link.TargetPath -ne (Join-Path $env:TRYOMARCHY_TEST_DIR 'TryOmarchy.exe')){throw 'Wrong target'}; if($link.WorkingDirectory -ne $env:TRYOMARCHY_TEST_DIR){throw 'Wrong directory'}; $expected='-dir "'+$env:TRYOMARCHY_TEST_DIR+'"'; if($name -eq 'Settings.lnk'){$expected+=' -settings'}; if($link.Arguments -ne $expected){throw 'Wrong arguments'} }`
	cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", script)
	cmd.Env = append(os.Environ(), "TRYOMARCHY_TEST_DIR="+dir)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("checking shortcuts: %v: %s", err, strings.TrimSpace(string(out)))
	}
	if !shortcutOfferRecorded(dir) {
		t.Fatal("restored launch could offer to replace original shortcuts")
	}
}
