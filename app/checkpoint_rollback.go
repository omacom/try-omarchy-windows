package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
)

const checkpointRollbackFile = ".snapshot-rollback.json"

type checkpointRollbackItem struct {
	Name     string `json:"name"`
	Previous bool   `json:"previous"`
	Next     bool   `json:"next"`
}

type checkpointRollbackState struct {
	Version   int                      `json:"version"`
	ID        string                   `json:"id"`
	Committed bool                     `json:"committed"`
	Items     []checkpointRollbackItem `json:"items"`
}

func checkpointRollbackNames() []string {
	names := []string{"guest", "runtime", "vm", "settings.json", storageSettingsFilename, desktopPreferencesFilename, audioPreferencesFilename, audioEndpointsFilename, resourcePreferencesFilename}
	for index := 0; index < maximumGuestDisplays; index++ {
		names = append(names, displayPlacementFilename(index))
	}
	return names
}

func rollbackPathExists(path string) (bool, error) {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if err := validateMovePath(path); err != nil {
		return false, err
	}
	if !info.IsDir() && !info.Mode().IsRegular() {
		return false, fmt.Errorf("unexpected snapshot recovery file: %s", path)
	}
	return true, nil
}

func saveCheckpointRollback(dir string, state checkpointRollbackState) error {
	f, err := os.CreateTemp(dir, ".snapshot-journal-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	err = json.NewEncoder(f).Encode(state)
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return publishMoveFile(f.Name(), filepath.Join(dir, checkpointRollbackFile))
}

// Recovery is idempotent after every rename, including interruptions during
// recovery itself. A durable commit keeps the restored state; every earlier
// interruption reinstates the old state. Neither side is deleted here.
func recoverCheckpointRollback(dir string) error {
	path := filepath.Join(dir, checkpointRollbackFile)
	present, err := rollbackPathExists(path)
	if err != nil || !present {
		return err
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	var state checkpointRollbackState
	dec := json.NewDecoder(io.LimitReader(f, 16385))
	dec.DisallowUnknownFields()
	err = dec.Decode(&state)
	if err == nil && dec.Decode(new(any)) != io.EOF {
		err = fmt.Errorf("invalid recovery journal")
	}
	f.Close()
	if err != nil {
		return err
	}
	names := checkpointRollbackNames()
	if state.Version != 1 || !validCheckpointID(state.ID) || len(state.Items) != len(names) {
		return fmt.Errorf("invalid snapshot recovery journal")
	}
	for index, item := range state.Items {
		if item.Name != names[index] {
			return fmt.Errorf("invalid snapshot recovery inventory")
		}
	}
	stage := filepath.Join(dir, ".snapshot-rollback-"+state.ID)
	if err := validateMovePath(stage); err != nil {
		return err
	}
	if !state.Committed {
		// A parent launcher holds the lifecycle lock. Also catch an orphaned
		// runtime before any directory moves, whichever side contains its disk.
		for _, base := range []string{dir, filepath.Join(stage, "data"), filepath.Join(stage, "next")} {
			for _, disk := range []string{"disk.raw", "disk.qcow2"} {
				path := filepath.Join(base, "vm", disk)
				exists, err := rollbackPathExists(path)
				if err != nil {
					return err
				}
				if !exists {
					continue
				}
				lock, err := openBackupDisk(path)
				if err != nil {
					return fmt.Errorf("close Omarchy before snapshot recovery: %w", err)
				}
				lock.Close()
			}
		}
		if err := undoCheckpointRollback(dir, stage, state, publishMoveDirectory); err != nil {
			return err
		}
	}
	return os.Remove(path)
}

func undoCheckpointRollback(dir, stage string, state checkpointRollbackState, rename func(string, string) error) error {
	// Validate the entire rename state before moving anything. A missing or
	// externally changed side must leave the journal available for recovery.
	for _, item := range state.Items {
		active, err := rollbackPathExists(filepath.Join(dir, item.Name))
		if err != nil {
			return err
		}
		next, err := rollbackPathExists(filepath.Join(stage, "next", item.Name))
		if err != nil {
			return err
		}
		previous, err := rollbackPathExists(filepath.Join(stage, "data", item.Name))
		if err != nil {
			return err
		}
		valid := false
		if item.Previous {
			if previous {
				valid = (next == item.Next && !active) || (item.Next && !next && active)
			} else {
				valid = active && next == item.Next
			}
		} else if !previous {
			valid = (next == item.Next && !active) || (item.Next && !next && active)
		}
		if !valid {
			return fmt.Errorf("snapshot recovery found missing or conflicting data for %s; retained files were kept", item.Name)
		}
	}
	for index := len(state.Items) - 1; index >= 0; index-- {
		item := state.Items[index]
		active := filepath.Join(dir, item.Name)
		next := filepath.Join(stage, "next", item.Name)
		previous := filepath.Join(stage, "data", item.Name)
		nextExists, err := rollbackPathExists(next)
		if err != nil {
			return err
		}
		if item.Next && !nextExists {
			if err := rename(active, next); err != nil {
				return err
			}
		}
		previousExists, err := rollbackPathExists(previous)
		if err != nil {
			return err
		}
		if item.Previous && previousExists {
			activeExists, err := rollbackPathExists(active)
			if err != nil {
				return err
			}
			if activeExists {
				return fmt.Errorf("snapshot recovery found conflicting data at %s", active)
			}
			if err := rename(previous, active); err != nil {
				return err
			}
		}
	}
	return nil
}

// Rollback replaces only the managed boot state. The snapshot catalog,
// launcher, host identity and unrelated files stay in their original location.
// The previous complete boot state is retained in the returned directory.
func (s checkpointStore) Rollback(id string, report backupProgress) (string, error) {
	tool := "qemu-img"
	if runtime.GOOS == "windows" {
		tool += ".exe"
	}
	return s.rollbackUsingTool(id, filepath.Join(s.installation, "runtime", "bin", tool), report)
}

func (s checkpointStore) rollbackUsingTool(id, tool string, report backupProgress) (string, error) {
	if err := recoverCheckpointRollback(s.installation); err != nil {
		return "", err
	}
	for _, name := range []string{payloadUpdateStateFilename, updateStateFilename} {
		if _, err := os.Lstat(filepath.Join(s.installation, name)); !os.IsNotExist(err) {
			return "", fmt.Errorf("finish the pending update before rolling back")
		}
	}
	inventory, err := inspectInstallationDisk(s.installation)
	if err != nil {
		return "", err
	}
	lock, err := openBackupDisk(inventory.Path)
	if err != nil {
		return "", fmt.Errorf("close Omarchy before rolling back: %w", err)
	}
	defer lock.Close()
	// Use a private temporary directory, then a journal identity independent
	// of the selected snapshot so repeated rollbacks preserve every recovery.
	temp, err := os.MkdirTemp(s.installation, ".snapshot-preparing-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(temp)
	next := filepath.Join(temp, "next")
	if err := s.restoreVerified(id, next, report); err != nil {
		return "", err
	}
	currentArch, err := checkpointGuestArchitecture(s.installation)
	if err != nil {
		return "", err
	}
	nextArch, err := checkpointGuestArchitecture(next)
	if err != nil {
		return "", err
	}
	if currentArch != nextArch {
		return "", fmt.Errorf("snapshot architecture %s does not match this installation (%s)", nextArch, currentArch)
	}
	if inventory.Format == "qcow2" {

		if err := makeRestoredDiskPortable(next, tool, report); err != nil {
			return "", err
		}
	}
	if err := markCheckpointBoot(next); err != nil {
		return "", err
	}
	state := checkpointRollbackState{Version: 1, ID: randomCheckpointRollbackID()}
	for _, name := range checkpointRollbackNames() {
		previous, err := rollbackPathExists(filepath.Join(s.installation, name))
		if err != nil {
			return "", err
		}
		replacement, err := rollbackPathExists(filepath.Join(next, name))
		if err != nil {
			return "", err
		}
		state.Items = append(state.Items, checkpointRollbackItem{name, previous, replacement})
	}
	if err := os.Mkdir(filepath.Join(temp, "data"), 0700); err != nil {
		return "", err
	}
	if err := checkSetupCancelled(); err != nil {
		return "", err
	}
	stage := filepath.Join(s.installation, ".snapshot-rollback-"+state.ID)
	if err := publishMoveDirectory(temp, stage); err != nil {
		return "", err
	}
	if err := saveCheckpointRollback(s.installation, state); err != nil {
		return "", err
	}
	if err := lock.Close(); err != nil {
		return "", err
	}
	for _, item := range state.Items {
		if item.Previous {
			err = publishMoveDirectory(filepath.Join(s.installation, item.Name), filepath.Join(stage, "data", item.Name))
		}
		if err == nil && item.Next {
			err = publishMoveDirectory(filepath.Join(stage, "next", item.Name), filepath.Join(s.installation, item.Name))
		}
		if err != nil {
			break
		}
	}
	if err == nil {
		state.Committed = true
		err = saveCheckpointRollback(s.installation, state)
	}
	if err != nil {
		recoveryErr := recoverCheckpointRollback(s.installation)
		if recoveryErr != nil {
			return "", fmt.Errorf("snapshot rollback: %v; recovery must finish before launch: %w", err, recoveryErr)
		}
		return "", err
	}
	if err := os.Remove(filepath.Join(s.installation, checkpointRollbackFile)); err != nil {
		return "", err
	}
	return filepath.Join(stage, "data"), nil
}

func randomCheckpointRollbackID() string {
	var token [16]byte
	rand.Read(token[:])
	return hex.EncodeToString(token[:])
}

func checkpointGuestArchitecture(dir string) (string, error) {
	f, err := os.Open(filepath.Join(dir, "guest", "build-spec.json"))
	if err != nil {
		return "", err
	}
	defer f.Close()
	var spec struct {
		Image struct {
			Architecture string `json:"architecture"`
		} `json:"image"`
	}
	if err := json.NewDecoder(io.LimitReader(f, 1<<20)).Decode(&spec); err != nil {
		return "", err
	}
	if spec.Image.Architecture != "x86_64" && spec.Image.Architecture != "aarch64" {
		return "", fmt.Errorf("unknown guest architecture")
	}
	return spec.Image.Architecture, nil
}
