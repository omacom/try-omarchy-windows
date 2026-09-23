//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestApproveWindowsExecutableRequiresAnExistingLocalExe(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "Sample.exe")
	if err := os.WriteFile(path, []byte("fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	prefs := approvedAppPreferences{SchemaVersion: 1}
	if err := approveWindowsExecutable(&prefs, path); err != nil {
		t.Fatal(err)
	}
	if len(prefs.Apps) != 1 || prefs.Apps[0].Name != "Sample" || !validApprovedAppID(prefs.Apps[0].ID) {
		t.Fatalf("unexpected approval %+v", prefs.Apps)
	}
	for _, reject := range []string{path, filepath.Join(dir, "missing.exe"), filepath.Join(dir, "notes.txt"), `\\server\share\tool.exe`} {
		if err := approveWindowsExecutable(&prefs, reject); err == nil {
			t.Errorf("accepted %q", reject)
		}
	}
}

func TestApprovedAppsSettingsNative(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires an interactive Windows desktop and a candidate launcher")
	}
	dir := t.TempDir()
	prefs := approvedAppPreferences{Apps: []approvedWindowsApp{{ID: strings.Repeat("a", 32), Name: "Example", Path: `C:\Example.exe`}}}
	if err := saveApprovedWindowsApps(dir, prefs); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(launcher, "-dir", dir, "-settings")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer cmd.Process.Kill()
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var window, list uintptr
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		if window != 0 {
			var owner uint32
			procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
			if owner == uint32(cmd.Process.Pid) {
				list, _, _ = user32.NewProc("GetDlgItem").Call(window, settingsAppListID)
				if list != 0 {
					break
				}
			}
		}
		time.Sleep(25 * time.Millisecond)
	}
	if list == 0 {
		t.Fatal("approved apps page did not appear")
	}
	procSendMessageW.Call(window, wmCommand, settingsPageBase+4, 0)
	if count, _, _ := procSendMessageW.Call(list, 0x18B, 0, 0); count != 1 { // LB_GETCOUNT
		t.Fatalf("approved app count=%d", count)
	}
	procSendMessageW.Call(list, 0x186, 0, 0) // LB_SETCURSEL
	procSendMessageW.Call(window, wmCommand, settingsAppRemoveID, 0)
	if count, _, _ := procSendMessageW.Call(list, 0x18B, 0, 0); count != 0 {
		t.Fatalf("removed app still listed: %d", count)
	}
	procPostMessageW.Call(window, wmCommand, settingsSaveID, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("approved apps settings did not save")
	}
	loaded, err := loadApprovedWindowsApps(dir)
	if err != nil || len(loaded.Apps) != 0 {
		t.Fatalf("removed app still approved: %+v, %v", loaded, err)
	}
}
