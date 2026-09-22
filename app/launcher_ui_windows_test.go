//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestLauncherMenuGuard(t *testing.T) {
	name := fmt.Sprintf("TryOmarchy-Test-%d", os.Getpid())
	first, err := acquireLauncherMenu(name)
	if err != nil || first == 0 {
		t.Fatalf("first lock: %d, %v", first, err)
	}
	defer procCloseHandle.Call(first)
	second, err := acquireLauncherMenu(name)
	if second != 0 {
		procCloseHandle.Call(second)
	}
	if err != nil || second != 0 {
		t.Fatalf("duplicate lock: %d, %v", second, err)
	}
	// A distinct test installation can still open a launcher independently.
	other, err := acquireLauncherMenu(name + "-other")
	if err != nil || other == 0 {
		t.Fatalf("independent lock: %d, %v", other, err)
	}
	procCloseHandle.Call(other)
}

// Exercise the actual executable in an isolated directory. Neither case boots
// a guest: Close must exit, and Settings must save without launching.
func TestLauncherWindowKeyboard(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and TRYOMARCHY_LAUNCHER_TEST_EXE")
	}
	for _, settingsOnly := range []bool{false, true} {
		name := "launcher Enter on Close"
		if settingsOnly {
			name = "settings Enter saves without boot"
		}
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			if settingsOnly {
				// The memory field only feeds settings in the Manual profile;
				// presets size the guest at launch instead.
				if err := saveResourcePreferences(dir, resourceManual); err != nil {
					t.Fatal(err)
				}
			}
			args := []string{"-dir", dir}
			titleText := appTitle
			if settingsOnly {
				args = append(args, "-settings")
				titleText += " settings"
			}
			cmd := exec.Command(launcher, args...)
			cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			done := make(chan error, 1)
			go func() { done <- cmd.Wait() }()
			defer func() { _ = cmd.Process.Kill() }()
			class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
			title, _ := syscall.UTF16PtrFromString(titleText)
			var window, target uintptr
			for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
				window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)))
				if window != 0 {
					var owner uint32
					procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
					if owner == uint32(cmd.Process.Pid) {
						id := uintptr(settingsCancelID)
						if settingsOnly {
							id = settingsMemID
						}
						target, _, _ = user32.NewProc("GetDlgItem").Call(window, id)
						if target != 0 {
							break
						}
					}
				}
				time.Sleep(25 * time.Millisecond)
			}
			if target == 0 {
				t.Fatal("launcher controls did not appear")
			}
			if settingsOnly {
				value, _ := syscall.UTF16PtrFromString("2")
				procSendMessageW.Call(target, wmSettext, 0, uintptr(unsafe.Pointer(value)))
			}
			procPostMessageW.Call(target, wmKeydown, 13, 0)
			select {
			case err := <-done:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(10 * time.Second):
				procPostMessageW.Call(window, wmClose, 0, 0)
				t.Fatal("Enter did not finish the window")
			}
			if settingsOnly {
				settings, err := loadSettings(settingsPath(dir))
				if err != nil || settings.MemoryMiB != 2048 {
					t.Fatalf("saved memory: %d, error: %v", settings.MemoryMiB, err)
				}
			} else if _, err := os.Stat(settingsPath(dir)); !os.IsNotExist(err) {
				t.Fatalf("Close unexpectedly wrote settings: %v", err)
			}
		})
	}
}

func TestAutomaticStartSettingNative(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and TRYOMARCHY_LAUNCHER_TEST_EXE")
	}
	dir := t.TempDir()
	cmd := exec.Command(launcher, "-dir", dir, "-settings")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer func() { _ = cmd.Process.Kill() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	title, _ := syscall.UTF16PtrFromString(appTitle + " settings")
	var window, checkbox uintptr
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)))
		if window != 0 {
			var owner uint32
			procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
			if owner == uint32(cmd.Process.Pid) {
				checkbox, _, _ = user32.NewProc("GetDlgItem").Call(window, settingsStartAutomaticallyID)
				if checkbox != 0 {
					break
				}
			}
		}
		time.Sleep(25 * time.Millisecond)
	}
	if checkbox == 0 {
		t.Fatal("automatic startup control did not appear")
	}
	procSendMessageW.Call(checkbox, 0x00f5, 0, 0) // BM_CLICK
	procSendMessageW.Call(window, wmCommand, settingsSaveID, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(10 * time.Second):
		procPostMessageW.Call(window, wmClose, 0, 0)
		t.Fatal("saving automatic startup did not close Settings")
	}
	prefs, err := loadLaunchPreferences(dir)
	if err != nil || !prefs.StartAutomatically {
		t.Fatalf("automatic startup preference = %#v, error: %v", prefs, err)
	}
}

func TestSnapshotsWindowHiddenStartup(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and TRYOMARCHY_LAUNCHER_TEST_EXE")
	}
	cmd := exec.Command(launcher, "-dir", t.TempDir(), "-recovery", "snapshots")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer func() { _ = cmd.Process.Kill() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchySnapshots")
	var window uintptr
	visible := false
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		if window != 0 {
			var owner uint32
			procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
			shown, _, _ := user32.NewProc("IsWindowVisible").Call(window)
			if owner == uint32(cmd.Process.Pid) && shown != 0 {
				visible = true
				break
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !visible {
		t.Fatal("Snapshots did not show its native window")
	}
	procPostMessageW.Call(window, wmClose, 0, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Snapshots did not exit after closing")
	}
}
