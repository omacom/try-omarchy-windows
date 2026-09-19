//go:build windows

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestNativeStableRestartHelper(t *testing.T) {
	marker := os.Getenv("TRYOMARCHY_STABLE_MARKER")
	if marker == "" {
		return
	}
	self, err := os.Executable()
	if err != nil {
		os.Exit(2)
	}
	data, err := json.Marshal(struct {
		PID  int
		Path string
	}{os.Getpid(), self})
	if err != nil {
		os.Exit(3)
	}
	if err := os.WriteFile(marker, data, 0600); err != nil {
		os.Exit(4)
	}
	os.Exit(0)
}

func TestNativeStableRollbackHelper(t *testing.T) {
	dir := os.Getenv("TRYOMARCHY_STABLE_ROLLBACK_DIR")
	if dir == "" {
		return
	}
	args, err := encodeRestartArgs([]string{"-test.run=^TestNativeStableRestartHelper$"})
	if err != nil {
		os.Exit(2)
	}
	if err = applyLauncherUpdate(dir, 2147483647, args, true); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	os.Exit(0)
}

// The signed feed uses a throwaway test key and native test executables. This
// verifies publication/rollback mechanics, not a public stable release or guest boot.
func TestNativeSignedStableUpdateAndRollback(t *testing.T) {
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	candidate, err := os.ReadFile(self)
	if err != nil {
		t.Fatal(err)
	}
	for _, version := range []string{"v1.0.0", "v1.0.1"} {
		t.Run(version, func(t *testing.T) {
			dir := t.TempDir()
			target := filepath.Join(dir, stableLauncherName)
			// A PE overlay gives the previous executable a distinct hash while keeping
			// it runnable, so rollback must restore the previous bytes, not the candidate.
			previous := append(append([]byte(nil), candidate...), []byte("previous launcher fixture")...)
			if err := os.WriteFile(target, previous, 0700); err != nil {
				t.Fatal(err)
			}
			sentinel := []byte(`{"schemaVersion":1,"cameraDisabled":true}`)
			if err := os.WriteFile(filepath.Join(dir, desktopPreferencesFilename), sentinel, 0600); err != nil {
				t.Fatal(err)
			}
			data := validUpdateJSON(version)
			var metadata updateManifest
			if err := json.Unmarshal(data, &metadata); err != nil {
				t.Fatal(err)
			}
			metadata.Launcher.SHA256 = testSHA256(candidate)
			data, err = json.Marshal(metadata)
			if err != nil {
				t.Fatal(err)
			}
			server, key := signedUpdateServer(t, data, false)
			defer server.Close()
			manifest, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key)
			if err != nil {
				t.Fatal(err)
			}
			state := &launcherUpdateState{Schema: updateStateVersion, Version: manifest.Version, SHA256: manifest.Launcher.SHA256}
			if err := writeLauncherUpdateState(dir, state); err != nil {
				t.Fatal(err)
			}
			marker := filepath.Join(dir, "started.txt")
			t.Setenv("TRYOMARCHY_STABLE_MARKER", marker)
			args, err := encodeRestartArgs([]string{"-test.run=^TestNativeStableRestartHelper$"})
			if err != nil {
				t.Fatal(err)
			}
			if err := applyLauncherUpdate(dir, 2147483647, args, false); err != nil {
				t.Fatal(err)
			}
			waitStarted := func() {
				t.Helper()
				deadline := time.Now().Add(10 * time.Second)
				for {
					data, err := os.ReadFile(marker)
					if err == nil {
						var started struct {
							PID  int
							Path string
						}
						if err := json.Unmarshal(data, &started); err != nil {
							t.Fatal(err)
						}
						if !pathsEqual(started.Path, target) || started.PID <= 0 {
							t.Fatalf("wrong restart: %s", data)
						}
						// Production passes the running launcher's PID. Wait for
						// this helper too before exercising replacement/rollback.
						waitForProcess(started.PID)
						return
					}
					if time.Now().After(deadline) {
						t.Fatal("launcher did not restart")
					}
					time.Sleep(25 * time.Millisecond)
				}
			}
			waitStarted()
			if ok, err := verifyFileSHA256(target, testSHA256(candidate), nil); err != nil || !ok {
				t.Fatal("candidate was not installed", err)
			}
			old, err := os.ReadFile(previousLauncherPath(dir))
			if err != nil || !bytes.Equal(old, previous) {
				t.Fatal("previous executable was not retained", err)
			}
			state, err = readLauncherUpdateState(dir)
			if err != nil || state == nil || !state.HasPrevious || state.Version != version {
				t.Fatal("rollback marker missing", err)
			}
			state.Started = true
			if err := writeLauncherUpdateState(dir, state); err != nil {
				t.Fatal(err)
			}
			if err := os.Remove(marker); err != nil {
				t.Fatal(err)
			}
			t.Setenv("TRYOMARCHY_STABLE_ROLLBACK_DIR", dir)
			cmd := exec.Command(previousLauncherPath(dir), "-test.run=^TestNativeStableRollbackHelper$")
			if output, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("rollback: %v: %s", err, output)
			}
			waitStarted()
			actual, err := os.ReadFile(target)
			if err != nil || !bytes.Equal(actual, previous) {
				t.Fatal("rollback did not restore original bytes", err)
			}
			state, err = readLauncherUpdateState(dir)
			if err != nil || state != nil {
				t.Fatal("rollback marker not cleared", err)
			}
			actual, err = os.ReadFile(filepath.Join(dir, desktopPreferencesFilename))
			if err != nil || !bytes.Equal(actual, sentinel) {
				t.Fatal("update or rollback changed preferences", err)
			}
		})
	}
}
