//go:build windows

package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestNativeEnvironmentSnapshot(t *testing.T) {
	for _, test := range []struct{ environment, expected []string }{
		{nil, nil}, {[]string{}, []string{}},
		{[]string{"OMARCHY_SDL_EXTERNAL_KEYBOARD=1", "SDL_AUDIODRIVER=directsound"}, []string{"OMARCHY_SDL_EXTERNAL_KEYBOARD=1", "SDL_AUDIODRIVER=directsound"}},
	} {
		original := exec.Command("unused-qemu.exe")
		original.Env = test.environment
		var prepared *exec.Cmd
		_, err := startSupervisedGuest(context.Background(), original, func(input *exec.Cmd) (*exec.Cmd, error) {
			if len(input.Env) > 0 {
				input.Env[0] = "changed-by-profile=1"
			}
			input.Env = []string{"replaced-by-profile=1"}
			prepared = exec.Command("must-not-run-owner.exe")
			prepared.SysProcAttr = &syscall.SysProcAttr{} // Reject before event/process creation.
			return prepared, nil
		})
		if err == nil || prepared.Process != nil || !reflect.DeepEqual(prepared.Env, test.expected) {
			t.Fatal("original nil/empty/value environment snapshot changed")
		}
	}
}

func TestNativeLauncherProfileEncoding(t *testing.T) {
	original := exec.Command(`C:\GPU 試験🙂\qemu.exe`, "-name", "Try Omarchy", "-append", "root=/dev/vda quiet")
	profile := nativeGPULauncherProfile{`C:\GPU 試験🙂\driver.exe`, `C:\GPU 試験🙂\audit`}
	before := append([]string(nil), original.Args...)
	prepared, err := profile.prepare(original)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(original.Args, before) {
		t.Fatal("profile mutated original arguments")
	}
	index := -1
	for i, value := range prepared.Args {
		if value == "--driver-launcher-profile" {
			index = i + 1
		}
		if value == "--driver-window-input" || value == "--driver-input-self-test" || value == "--driver-rootfs-readonly" {
			t.Fatal("profile replaced the official input/boot path")
		}
	}
	if index < 0 {
		t.Fatal("profile missing")
	}
	for _, value := range prepared.Args[index] {
		if value >= 128 {
			t.Fatal("native char argv is not ASCII")
		}
	}
	var decoded struct {
		QEMU      string   `json:"qemu"`
		Arguments []string `json:"arguments"`
	}
	if err := json.Unmarshal([]byte(prepared.Args[index]), &decoded); err != nil || decoded.QEMU != original.Path || !reflect.DeepEqual(decoded.Arguments, before[1:]) {
		t.Fatal("Unicode argument roundtrip changed the original command")
	}
	if _, err := (nativeGPULauncherProfile{}).prepare(original); err == nil {
		t.Fatal("relative/empty profile accepted")
	}
	tooLarge := exec.Command(original.Path, strings.Repeat("x", 20001))
	if _, err := profile.prepare(tooLarge); err == nil {
		t.Fatal("unbounded profile accepted")
	}
	quoted := exec.Command(original.Path, strings.Repeat("\"", 9000))
	if _, err := profile.prepare(quoted); err == nil {
		t.Fatal("Windows quoting expansion was not bounded")
	}
	invalid := exec.Command(original.Path, "bad\x00value")
	if _, err := profile.prepare(invalid); err == nil {
		t.Fatal("NUL profile accepted")
	}
	_, err = startSupervisedGuest(context.Background(), original, func(*exec.Cmd) (*exec.Cmd, error) { return nil, errors.New("profile unavailable") })
	if err == nil || original.Process != nil {
		t.Fatal("unprepared profile started a child")
	}
}

func TestNativeLauncherProfileExport(t *testing.T) {
	path := os.Getenv("OMARCHY_NATIVE_LAUNCHER_PROFILE_OUTPUT")
	if path == "" {
		t.Skip("explicit CPU-only profile export")
	}
	type row struct {
		Name      string   `json:"name"`
		Supported bool     `json:"supported"`
		Original  []string `json:"original"`
		Encoded   string   `json:"encoded"`
		Prepared  []string `json:"prepared"`
	}
	var rows []row
	for _, name := range []string{"cursor-off", "cursor-on", "unicode", "cpu", "venus", "multiple-displays"} {
		cfg := &config{useGpu: true, memMiB: 6144, cpus: 4, hostTotalMiB: 16384, displays: 1, audio: "none", diskFormat: "qcow2",
			disk: `C:\Native GPU Fixture\root.qcow2`, guestDir: `C:\Native GPU Fixture\guest`, vmDir: `C:\Native GPU Fixture\vm`, qmpDir: `C:\Native GPU Fixture\controls`}
		qemu := `C:\Native GPU Fixture\qemu.exe`
		audit := `C:\Native GPU Fixture\audit`
		switch name {
		case "cursor-on":
			cfg.hostCursor = true
		case "unicode":
			cfg.disk = `C:\GPU 試験🙂\root.qcow2`
			cfg.guestDir = `C:\GPU 試験🙂\guest`
			cfg.vmDir = `C:\GPU 試験🙂\vm`
			cfg.qmpDir = `C:\GPU 試験🙂\controls`
			qemu = `C:\GPU 試験🙂\qemu.exe`
			audit = `C:\GPU 試験🙂\audit`
		case "cpu":
			cfg.useGpu = false
		case "venus":
			cfg.venus = true
		case "multiple-displays":
			cfg.displays = 2
		}
		original := exec.Command(qemu, buildQemuArgs(cfg, "root=/dev/vda quiet")...)
		profile := nativeGPULauncherProfile{`C:\Native GPU Fixture\driver.exe`, audit}
		prepared, err := profile.prepare(original)
		if err != nil {
			t.Fatal(err)
		}
		encoded := ""
		for i, value := range prepared.Args {
			if value == "--driver-launcher-profile" {
				encoded = prepared.Args[i+1]
			}
		}
		rows = append(rows, row{name, name == "cursor-off" || name == "cursor-on" || name == "unicode", original.Args[1:], encoded, prepared.Args})
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.NewEncoder(file).Encode(rows); err != nil {
		file.Close()
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestNativeLauncherBindingFixture(t *testing.T) {
	fixture := os.Getenv("OMARCHY_NATIVE_BINDING_FIXTURE")
	if fixture == "" {
		t.Skip("explicit hidden CPU-only process fixture")
	}
	for _, mode := range []string{"ready", "exit-before-ready", "cancel-before-ready", "bad-ready", "duplicate-bound"} {
		t.Run(mode, func(t *testing.T) {
			directory := t.TempDir()
			marker := filepath.Join(directory, "allow-ready")
			log, err := os.Create(filepath.Join(directory, "qemu-stderr.log"))
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			original := exec.Command(fixture)
			original.Stdout, original.Stderr = log, log
			guest, err := startSupervisedGuest(ctx, original, func(*exec.Cmd) (*exec.Cmd, error) { return exec.Command(fixture, mode, marker), nil })
			if err != nil {
				log.Close()
				t.Fatal(err)
			}
			log.Close() // The reader's own log entry must outlive the caller.
			publishSupervisedGuest(guest)
			published := qemuPid.Load()
			owner := guest.owner
			owner.Lock()
			bound, control := owner.bindingSeen, owner.controlReady
			identity := owner.identity
			owner.Unlock()
			if !bound || control || !identity.Bound || identity.Ready || identity.QMP || published == 0 || published != identity.QEMUPID || published == identity.BridgePID {
				t.Fatal("early binding was not published before simulated QMP readiness")
			}
			if mode == "cancel-before-ready" {
				if err := stopSupervisedGuest(published); err != nil {
					t.Fatal(err)
				}
			} else {
				if err := os.WriteFile(marker, []byte("owned-controller\n"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "ready" {
				deadline := time.Now().Add(5 * time.Second)
				for time.Now().Before(deadline) {
					owner.Lock()
					control = owner.controlReady
					owner.Unlock()
					if control {
						break
					}
					time.Sleep(5 * time.Millisecond)
				}
				if !control {
					t.Fatal("later readiness did not retain the same VM")
				}
				if err := stopSupervisedGuest(published); err != nil {
					t.Fatal(err)
				}
			}
			if err := guest.wait(); err == nil {
				t.Fatal("forced helper exit was accepted as natural")
			}
			if !owner.qemuReaped || owner.qemuExit != 90 || owner.event != 0 {
				t.Fatal("held helper exit/cancellation cleanup not verified")
			}
			if mode == "exit-before-ready" && !memoryStarved(&config{vmDir: directory}) {
				t.Fatal("original early-exit memory classifier lost the startup error")
			}
			if (mode == "bad-ready" || mode == "duplicate-bound") && owner.failed == nil {
				t.Fatal("altered/duplicate binding transition accepted")
			}
		})
	}
}
