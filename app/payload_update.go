package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

const (
	payloadUpdateStateVersion  = 1
	payloadUpdateStateFilename = "payload-update-state.json"
	maxPayloadUpdateStateBytes = 64 << 10
)

type payloadUpdateState struct {
	Schema         int    `json:"schema"`
	Version        string `json:"version"`
	GuestPending   bool   `json:"guestPending"`
	RuntimePending bool   `json:"runtimePending"`
	Started        bool   `json:"started"`
}

func publishDirectoryUpdate(current, staged, previous string) error {
	if err := os.RemoveAll(previous); err != nil {
		return err
	}
	hadCurrent := false
	if info, err := os.Stat(current); err == nil {
		if !info.IsDir() {
			return fmt.Errorf("update target is not a directory: %s", current)
		}
		if err := renameDirectoryWithRetry(current, previous); err != nil {
			return err
		}
		hadCurrent = true
	} else if !os.IsNotExist(err) {
		return err
	}
	if err := renameDirectoryWithRetry(staged, current); err != nil {
		if hadCurrent {
			if restoreErr := renameDirectoryWithRetry(previous, current); restoreErr != nil {
				return fmt.Errorf("publishing update: %v; restoring previous files: %w", err, restoreErr)
			}
		}
		return err
	}
	return nil
}

func rollbackDirectoryUpdate(current, previous, failed string) error {
	if _, err := os.Stat(previous); os.IsNotExist(err) {
		return nil
	} else if err != nil {
		return err
	}
	_ = os.RemoveAll(failed)
	if _, err := os.Stat(current); err == nil {
		if err := renameDirectoryWithRetry(current, failed); err != nil {
			return err
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	if err := renameDirectoryWithRetry(previous, current); err != nil {
		_ = renameDirectoryWithRetry(failed, current)
		return err
	}
	_ = os.RemoveAll(failed)
	return nil
}

func renameDirectoryWithRetry(from, to string) error {
	var err error
	for attempt := 0; attempt < 15; attempt++ {
		if err = os.Rename(from, to); err == nil {
			return nil
		}
		if attempt < 14 {
			time.Sleep(200 * time.Millisecond)
		}
	}
	return err
}

func readPayloadUpdateState(dir string) (*payloadUpdateState, error) {
	data, err := os.ReadFile(filepath.Join(dir, payloadUpdateStateFilename))
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(data) > maxPayloadUpdateStateBytes {
		return nil, fmt.Errorf("payload update state is too large")
	}
	var state payloadUpdateState
	if err := json.Unmarshal(data, &state); err != nil {
		return nil, err
	}
	if state.Schema != payloadUpdateStateVersion {
		return nil, fmt.Errorf("payload update state is invalid")
	}
	if _, ok := parseReleaseVersion(state.Version); !ok {
		return nil, fmt.Errorf("payload update version is invalid")
	}
	return &state, nil
}

func writePayloadUpdateState(dir string, state *payloadUpdateState) error {
	data, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := filepath.Join(dir, payloadUpdateStateFilename)
	staged := path + ".part"
	f, err := os.OpenFile(staged, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			f.Close()
			os.Remove(staged)
		}
	}()
	if _, err := f.Write(data); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Rename(staged, path); err != nil {
		return err
	}
	ok = true
	return nil
}

func recordPayloadUpdate(dir, version string, guest, runtime bool) error {
	state, err := readPayloadUpdateState(dir)
	if err != nil || state == nil || state.Version != version {
		state = &payloadUpdateState{Schema: payloadUpdateStateVersion, Version: version}
	}
	state.GuestPending = state.GuestPending || guest
	state.RuntimePending = state.RuntimePending || runtime
	state.Started = true
	return writePayloadUpdateState(dir, state)
}

func rollbackPendingPayloadUpdates(dir string) (bool, error) {
	state, err := readPayloadUpdateState(dir)
	if err != nil {
		return false, err
	}
	if state == nil {
		return false, nil
	}
	if !state.Started {
		state.Started = true
		return false, writePayloadUpdateState(dir, state)
	}
	rolledBack := false
	if state.GuestPending {
		if err := rollbackDirectoryUpdate(filepath.Join(dir, "guest"), filepath.Join(dir, "guest.previous"), filepath.Join(dir, "guest.failed")); err != nil {
			return false, err
		}
		rolledBack = true
	}
	if state.RuntimePending {
		if err := rollbackDirectoryUpdate(filepath.Join(dir, "runtime"), filepath.Join(dir, "runtime.previous"), filepath.Join(dir, "runtime.failed")); err != nil {
			return false, err
		}
		rolledBack = true
	}
	if err := os.Remove(filepath.Join(dir, payloadUpdateStateFilename)); err != nil && !os.IsNotExist(err) {
		return false, err
	}
	return rolledBack, nil
}

// pinRestoredPayloads prevents a failed payload from being selected again in
// the same recovery launch. The next ordinary launch can check for updates
// again after the restored guest has had one chance to boot.
func pinRestoredPayloads(dir string, guestRelease, guestManifest, runtimeRelease, runtimeManifest *string) error {
	release, manifest, ok := installReceiptIdentity(filepath.Join(dir, "guest"))
	if !ok {
		return fmt.Errorf("restored guest install state is missing or invalid")
	}
	*guestRelease = release
	*guestManifest = manifest
	if release, manifest, ok := runtimeReceiptIdentity(filepath.Join(dir, "runtime")); ok {
		*runtimeRelease = release
		*runtimeManifest = manifest
	}
	return nil
}

func commitGuestPayloadUpdate(dir string) {
	state, err := readPayloadUpdateState(dir)
	if err != nil || state == nil || !state.GuestPending {
		return
	}
	_ = os.RemoveAll(filepath.Join(dir, "guest.previous"))
	state.GuestPending = false
	if state.RuntimePending {
		if err := writePayloadUpdateState(dir, state); err != nil {
			logf("recording successful guest update: %v", err)
		}
		return
	}
	if err := os.Remove(filepath.Join(dir, payloadUpdateStateFilename)); err != nil && !os.IsNotExist(err) {
		logf("clearing successful guest update: %v", err)
		return
	}
	logf("guest update %s confirmed after userspace reported ready", state.Version)
}

func commitRuntimePayloadUpdate(dir string) {
	state, err := readPayloadUpdateState(dir)
	if err != nil || state == nil || !state.RuntimePending {
		return
	}
	_ = os.RemoveAll(filepath.Join(dir, "runtime.previous"))
	state.RuntimePending = false
	if state.GuestPending {
		if err := writePayloadUpdateState(dir, state); err != nil {
			logf("recording successful runtime update: %v", err)
		}
		return
	}
	if err := os.Remove(filepath.Join(dir, payloadUpdateStateFilename)); err != nil && !os.IsNotExist(err) {
		logf("clearing successful runtime update: %v", err)
		return
	}
	logf("runtime update %s confirmed after guest userspace reported ready", state.Version)
}

// Normal launch commits every payload only after the guest's userspace
// readiness service reaches the lifecycle listener.
func commitPayloadUpdates(dir string) {
	commitRuntimePayloadUpdate(dir)
	commitGuestPayloadUpdate(dir)
}

func rollbackPendingRuntimeUpdate(dir string) (bool, error) {
	state, err := readPayloadUpdateState(dir)
	if err != nil || state == nil || !state.RuntimePending {
		return false, err
	}
	if err := rollbackDirectoryUpdate(filepath.Join(dir, "runtime"), filepath.Join(dir, "runtime.previous"), filepath.Join(dir, "runtime.failed")); err != nil {
		return false, err
	}
	state.RuntimePending = false
	if !state.GuestPending {
		return true, os.Remove(filepath.Join(dir, payloadUpdateStateFilename))
	}
	return true, writePayloadUpdateState(dir, state)
}
