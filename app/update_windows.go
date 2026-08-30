//go:build windows

package main

import (
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"time"
)

var procOpenProcess = kernel32.NewProc("OpenProcess")

const synchronizeProcess = 0x00100000

func maybeStartLauncherUpdate(cfg *config, updateURL string, restartArgs []string) (bool, error) {
	key, err := updatePublicKey()
	if err != nil {
		return false, err
	}
	metadataClient := &http.Client{
		Transport: &http.Transport{
			Proxy:                 http.ProxyFromEnvironment,
			DialContext:           (&net.Dialer{Timeout: 4 * time.Second}).DialContext,
			TLSHandshakeTimeout:   4 * time.Second,
			ResponseHeaderTimeout: 4 * time.Second,
		},
		Timeout: 10 * time.Second,
	}
	manifest, err := fetchUpdateManifest(metadataClient, updateURL, key)
	if err != nil {
		return false, err
	}
	if !updateIsNewer(manifest.Version, currentVersion) {
		return false, nil
	}
	ui := getUI()
	ui.setStatus("Updating Try Omarchy to %s...", manifest.Version)
	staged := stagedLauncherPath(cfg.dir, manifest.Version)
	if err := os.MkdirAll(filepath.Dir(staged), 0o755); err != nil {
		return false, err
	}
	downloadClient := &http.Client{Timeout: 0}
	if _, err := releaseSums(downloadClient, manifest.Release, manifest.ManifestSHA256); err != nil {
		return false, fmt.Errorf("authenticating updated guest manifest: %w", err)
	}
	launcherURL := normalizedRelease(manifest.Release) + "/" + manifest.Launcher.Name
	if err := ensureVerifiedDownload(downloadClient, launcherURL, staged, manifest.Launcher.SHA256,
		"Downloading a trusted Try Omarchy update...", ui); err != nil {
		return false, err
	}
	encodedArgs, err := encodeRestartArgs(restartArgs)
	if err != nil {
		return false, err
	}
	state := &launcherUpdateState{
		Schema: updateStateVersion, Version: manifest.Version,
		SHA256: manifest.Launcher.SHA256,
	}
	if err := writeLauncherUpdateState(cfg.dir, state); err != nil {
		return false, err
	}
	cmd := exec.Command(staged,
		"-dir", cfg.dir,
		"-apply-launcher-update",
		"-update-wait-pid", strconv.Itoa(os.Getpid()),
		"-update-restart-args", encodedArgs,
	)
	if err := cmd.Start(); err != nil {
		return false, err
	}
	return true, nil
}

func applyLauncherUpdate(dir string, waitPID int, encodedArgs string, rollback bool) error {
	if waitPID <= 0 {
		return fmt.Errorf("invalid update parent process")
	}
	args, err := decodeRestartArgs(encodedArgs)
	if err != nil {
		return fmt.Errorf("decode restart arguments: %w", err)
	}
	waitForProcess(waitPID)
	self, err := os.Executable()
	if err != nil {
		return err
	}
	target := filepath.Join(dir, stableLauncherName)
	if rollback {
		if err := copyLauncher(self, target, replaceLauncher); err != nil {
			return fmt.Errorf("restore previous launcher: %w", err)
		}
		// This process is running from the previous-launcher backup, so Windows
		// will not let us delete that file until it exits. Removing the marker is
		// enough to make the restored launcher authoritative; a later successful
		// update cleans the old backup.
		if err := clearLauncherUpdateMarker(dir); err != nil {
			return err
		}
	} else {
		state, err := readLauncherUpdateState(dir)
		if err != nil {
			return fmt.Errorf("read pending update state: %w", err)
		}
		if state == nil {
			return fmt.Errorf("pending update state is missing")
		}
		if ok, err := verifyFileSHA256(self, state.SHA256, nil); err != nil || !ok {
			return fmt.Errorf("staged launcher authentication failed")
		}
		if _, err := os.Stat(target); err == nil {
			if err := copyLauncher(target, previousLauncherPath(dir), replaceLauncher); err != nil {
				return fmt.Errorf("back up launcher: %w", err)
			}
			state.HasPrevious = true
		}
		// Commit the rollback marker before replacing the working launcher. A
		// power loss after the replacement must still leave enough information
		// for the new launcher to restore the previous signed executable.
		if err := writeLauncherUpdateState(dir, state); err != nil {
			return err
		}
		if err := copyLauncher(self, target, replaceLauncher); err != nil {
			if state.HasPrevious {
				_ = copyLauncher(previousLauncherPath(dir), target, replaceLauncher)
			}
			_ = clearLauncherUpdateMarker(dir)
			return err
		}
	}
	cmd := exec.Command(target, args...)
	if err := cmd.Start(); err != nil {
		if !rollback {
			_ = copyLauncher(previousLauncherPath(dir), target, replaceLauncher)
		}
		return fmt.Errorf("restart updated launcher: %w", err)
	}
	return nil
}

func recoverLauncherUpdate(dir, encodedArgs string) (bool, error) {
	state, err := readLauncherUpdateState(dir)
	if err != nil {
		// A damaged local state file must not brick an otherwise working app.
		_ = clearLauncherUpdateState(dir)
		return false, err
	}
	if state == nil || state.Version != currentVersion {
		return false, nil
	}
	if !state.Started {
		state.Started = true
		return false, writeLauncherUpdateState(dir, state)
	}
	if !state.HasPrevious {
		_ = clearLauncherUpdateState(dir)
		return false, nil
	}
	previous := previousLauncherPath(dir)
	cmd := exec.Command(previous,
		"-dir", dir,
		"-apply-launcher-rollback",
		"-update-wait-pid", strconv.Itoa(os.Getpid()),
		"-update-restart-args", encodedArgs,
	)
	if err := cmd.Start(); err != nil {
		return false, err
	}
	return true, nil
}

func commitLauncherUpdate(dir string) {
	state, err := readLauncherUpdateState(dir)
	if err != nil || state == nil || state.Version != currentVersion {
		return
	}
	if err := clearLauncherUpdateState(dir); err != nil {
		logf("clearing successful launcher update: %v", err)
		return
	}
	_ = os.RemoveAll(filepath.Join(launcherUpdateDir(dir), currentVersion))
	logf("launcher update %s confirmed after healthy boot", currentVersion)
}

func waitForProcess(pid int) {
	handle, _, _ := procOpenProcess.Call(synchronizeProcess, 0, uintptr(uint32(pid)))
	if handle == 0 {
		time.Sleep(2 * time.Second)
		return
	}
	defer procCloseHandle.Call(handle)
	procWaitForSingleObject.Call(handle, uintptr(0xFFFFFFFF))
}
