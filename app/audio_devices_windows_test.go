//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestNativeAudioDeviceEnumeration(t *testing.T) {
	qemu := os.Getenv("TRYOMARCHY_AUDIO_TEST_QEMU")
	if qemu == "" {
		t.Skip("requires installed runtime and Windows audio endpoints")
	}
	for i := 0; i < 2; i++ {
		catalog, err := listAudioDevices(qemu)
		if err != nil {
			t.Fatal(err)
		}
		t.Logf("SDL audio catalog: output=%q input=%q", catalog.Output, catalog.Input)
		if len(catalog.Output) == 0 {
			t.Fatal("test laptop has no output endpoint")
		}
		for _, names := range [][]string{catalog.Output, catalog.Input} {
			seen := map[string]bool{}
			for _, name := range names {
				if name == "" || seen[name] {
					t.Fatalf("invalid/duplicate SDL name %q", name)
				}
				seen[name] = true
			}
		}
	}
}

func TestNativeStableAudioEndpointEnumeration(t *testing.T) {
	qemu := os.Getenv("TRYOMARCHY_AUDIO_TEST_QEMU")
	if qemu == "" {
		t.Skip("requires installed runtime and Windows audio endpoints")
	}
	endpoints, err := listAudioEndpoints()
	if err != nil {
		t.Fatal(err)
	}
	catalog, err := listAudioDevices(qemu)
	if err != nil {
		t.Fatal(err)
	}
	for direction, row := range []struct {
		endpoints []audioEndpointInfo
		names     []string
	}{{endpoints.Output, catalog.Output}, {endpoints.Input, catalog.Input}} {
		if direction == 0 && len(row.endpoints) == 0 {
			t.Fatal("Windows reported no active output endpoints")
		}
		seen := map[string]bool{}
		matchingNames := 0
		for _, endpoint := range row.endpoints {
			if endpoint.ID == "" || endpoint.Name == "" || seen[endpoint.ID] {
				t.Fatalf("invalid or duplicate endpoint: %+v", endpoint)
			}
			seen[endpoint.ID] = true
			for _, name := range row.names {
				if endpoint.Name == name {
					matchingNames++
				}
			}
		}
		if len(row.names) > 0 && matchingNames == 0 {
			t.Fatalf("SDL names %q do not match stable endpoint names %+v", row.names, row.endpoints)
		}
	}
}

// Exercise real controls and persistence against the selected test runtime.
// With v20 the choices must be disabled; with r16 both directions must save.
func TestAudioSettingsNative(t *testing.T) {
	qemu, launcher := os.Getenv("TRYOMARCHY_AUDIO_TEST_QEMU"), os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || qemu == "" || launcher == "" {
		t.Skip("requires interactive Windows, launcher and runtime")
	}
	catalog, err := listAudioDevices(qemu)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	supported := audioRuntimeSupportsSelection(qemu)
	stable, err := listAudioEndpoints()
	if err != nil {
		t.Fatal(err)
	}
	for _, defaults := range []bool{false, true} {
		cmd := exec.Command(launcher, "-dir", dir, "-settings", "-winq", filepath.Dir(filepath.Dir(qemu)))
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		defer cmd.Process.Kill()
		class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
		var window uintptr
		controls := map[uintptr]uintptr{}
		for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
			window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
			if window != 0 {
				var owner uint32
				procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
				if owner == uint32(cmd.Process.Pid) {
					for _, id := range []uintptr{settingsAudioOutputID, settingsAudioInputID, settingsSaveID} {
						controls[id], _, _ = user32.NewProc("GetDlgItem").Call(window, id)
					}
					if controls[settingsAudioInputID] != 0 && controls[settingsSaveID] != 0 {
						break
					}
				}
			}
			time.Sleep(25 * time.Millisecond)
		}
		if controls[settingsAudioInputID] == 0 {
			t.Fatal("audio controls did not appear")
		}
		for _, row := range []struct {
			id    uintptr
			names []string
		}{
			{settingsAudioOutputID, catalog.Output}, {settingsAudioInputID, catalog.Input},
		} {
			h := controls[row.id]
			enabled, _, _ := user32.NewProc("IsWindowEnabled").Call(h)
			if (enabled != 0) != supported {
				t.Fatalf("control %d enabled=%v runtime supports=%v", row.id, enabled != 0, supported)
			}
			count, _, _ := procSendMessageW.Call(h, 0x146, 0, 0)
			if count != uintptr(len(row.names)+1) {
				t.Fatalf("control %d count=%d catalog=%v", row.id, count, row.names)
			}
			if defaults && supported && len(row.names) > 0 {
				selected, _, _ := procSendMessageW.Call(h, 0x147, 0, 0)
				if selected != 1 {
					t.Fatal("saved choice did not survive reopening")
				}
			}
			index := uintptr(0)
			if !defaults && len(row.names) > 0 {
				index = 1
			}
			procSendMessageW.Call(h, 0x14E, index, 0)
		}
		procPostMessageW.Call(controls[settingsSaveID], 0xF5, 0, 0)
		select {
		case err := <-done:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("audio settings did not save")
		}
		prefs, err := loadAudioPreferences(dir)
		if err != nil {
			t.Fatal(err)
		}
		want := audioPreferences{SchemaVersion: 1}
		if supported && !defaults {
			if len(catalog.Output) > 0 {
				want.Output = catalog.Output[0]
			}
			if len(catalog.Input) > 0 {
				want.Input = catalog.Input[0]
			}
		}
		if prefs != want {
			t.Fatalf("saved %+v want %+v", prefs, want)
		}
		endpointPrefs, err := loadAudioEndpoints(dir)
		if err != nil {
			t.Fatal(err)
		}
		wantEndpoints := audioEndpoints{SchemaVersion: 1}
		if supported && !defaults {
			wantEndpoints.OutputID = endpointIDForSelection(want.Output, "", "", stable.Output)
			wantEndpoints.InputID = endpointIDForSelection(want.Input, "", "", stable.Input)
		}
		if endpointPrefs != wantEndpoints {
			t.Fatalf("saved endpoints %+v want %+v", endpointPrefs, wantEndpoints)
		}
	}
}
