package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func moveFixture(t *testing.T) (moveStore, string, string) {
	t.Helper()
	source, _ := backupFixture(t)
	root := filepath.Dir(source)
	return moveStore{dir: filepath.Join(root, "host"), defaultDir: source}, source, filepath.Join(root, "Other drive Ω", "TryOmarchy")
}

func prepareFixtureMove(t *testing.T, s moveStore, source, destination string) *installationMove {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		t.Fatal(err)
	}
	m, err := s.prepare(source, destination, nil)
	if err != nil {
		t.Fatal(err)
	}
	return m
}

func TestMoveVolumeIdentificationErrorKeepsLaunching(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, err := s.load()
	if err != nil {
		t.Fatal(err)
	}
	state.Locations = nil
	state.Retained.Volume = nil
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	original := identifyMoveVolume
	t.Cleanup(func() { identifyMoveVolume = original })
	calls := 0
	identifyMoveVolume = func(path string) (*moveVolumeLocation, error) {
		calls++
		if path != destination {
			t.Fatalf("identified %q instead of existing destination", path)
		}
		return nil, errors.New("volume GUID unavailable")
	}
	state, err = s.rememberVolume(state, source)
	if err != nil || calls != 1 {
		t.Fatalf("identity upgrade blocked startup: calls=%d err=%v", calls, err)
	}
	state, err = s.relocate(state, source, func(*installationMove) error {
		t.Fatal("attempted to relocate without volume identity")
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if resolved, err := resolveMovedDirectory(state, source); err != nil || resolved != destination {
		t.Fatalf("launch redirect=%q err=%v", resolved, err)
	}
	saved, err := s.load()
	if err != nil || saved.Redirects[source] != destination || len(saved.Locations) != 0 || saved.Retained.Volume != nil {
		t.Fatalf("existing history changed: %+v err=%v", saved, err)
	}
}

func TestMoveVolumeIdentificationFailureHasRecoveryGuidance(t *testing.T) {
	s, source, destination := moveFixture(t)
	if err := os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		t.Fatal(err)
	}
	original := identifyMoveVolume
	t.Cleanup(func() { identifyMoveVolume = original })
	failure := errors.New("volume GUID unavailable")
	identifyMoveVolume = func(string) (*moveVolumeLocation, error) { return nil, failure }
	_, err := s.prepare(source, destination, nil)
	if !errors.Is(err, failure) || !strings.Contains(err.Error(), destination) || !strings.Contains(err.Error(), "another local drive") {
		t.Fatalf("missing move recovery guidance: %v", err)
	}
	state, err := s.load()
	if err != nil || state.Pending != nil {
		t.Fatalf("failed identification started a move: %+v err=%v", state, err)
	}
}

func TestMovePreservesContentsCapacityTimestampsAndRedirects(t *testing.T) {
	s, source, destination := moveFixture(t)
	for _, name := range []string{"my-notes.txt", "vm/before-reset-old/disk.raw", approvedAppsFilename, approvedURLSchemesFilename, approvedFileTypesFilename} {
		path := filepath.Join(source, name)
		os.MkdirAll(filepath.Dir(path), 0700)
		if err := os.WriteFile(path, []byte("keep this too"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	m := prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{approvedAppsFilename, approvedURLSchemesFilename, approvedFileTypesFilename} {
		data, err := os.ReadFile(filepath.Join(destination, name))
		if err != nil || string(data) != "keep this too" {
			t.Fatalf("approval file %s was not retained: %q, %v", name, data, err)
		}
	}
	for _, entry := range m.Files {
		if entry.Directory {
			continue
		}
		want, err := os.ReadFile(filepath.Join(source, entry.Name))
		if err != nil {
			t.Fatal(err)
		}
		got, err := os.ReadFile(filepath.Join(destination, entry.Name))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(want, got) {
			t.Fatalf("changed %s", entry.Name)
		}
		info, err := os.Stat(filepath.Join(destination, entry.Name))
		if err != nil {
			t.Fatal(err)
		}
		if info.ModTime().UnixNano() != entry.ModTime {
			t.Fatalf("lost receipt timestamp: %s", entry.Name)
		}
	}
	state, err := s.load()
	if err != nil {
		t.Fatal(err)
	}
	resolved, err := resolveMovedDirectory(state, source)
	if err != nil || resolved != destination {
		t.Fatalf("redirect: %q %v", resolved, err)
	}
	pointed, ok, err := loadDataLocationPointer(source)
	if err != nil || !ok || pointed != destination {
		t.Fatalf("pointer: %q %t %v", pointed, ok, err)
	}
	if err := s.cleanup(destination); err == nil {
		t.Fatal("cleaned before guest boot")
	}
	if err := s.markBooted(destination); err != nil {
		t.Fatal(err)
	}
	if err := s.cleanup(destination); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); !os.IsNotExist(err) {
		t.Fatal("source disk retained after cleanup")
	}
	if _, ok, err := loadDataLocationPointer(source); err != nil || !ok {
		t.Fatalf("cleanup removed bootstrap pointer: %v", err)
	}
	if _, err := os.Stat(filepath.Join(destination, "vm", "disk.raw")); err != nil {
		t.Fatal(err)
	}
}

func TestMoveBackToDefaultFlattensRedirects(t *testing.T) {
	s, source, destination := moveFixture(t)
	hostMarker := filepath.Join(source, "portable-host", "keep.txt")
	if err := os.MkdirAll(filepath.Dir(hostMarker), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(hostMarker, []byte("host-only"), 0600); err != nil {
		t.Fatal(err)
	}
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	if err := s.markBooted(destination); err != nil {
		t.Fatal(err)
	}
	if err := s.cleanup(destination); err != nil {
		t.Fatal(err)
	}
	prepareFixtureMove(t, s, destination, source)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, err := s.load()
	if err != nil {
		t.Fatal(err)
	}
	resolved, err := resolveMovedDirectory(state, destination)
	if err != nil || resolved != source {
		t.Fatalf("redirect: %s %v", resolved, err)
	}
	resolved, err = resolveMovedDirectory(state, source)
	if err != nil || resolved != source {
		t.Fatalf("default redirected away: %s %v", resolved, err)
	}
	if _, ok, err := loadDataLocationPointer(source); err != nil || ok {
		t.Fatalf("self pointer retained: %t %v", ok, err)
	}
	if data, err := os.ReadFile(hostMarker); err != nil || string(data) != "host-only" {
		t.Fatalf("lost portable host state: %v", err)
	}
}

func TestMoveCleanupRejectsRunningOriginal(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	s.markBooted(destination)
	disk, err := openBackupDisk(filepath.Join(source, "vm", "disk.raw"))
	if err != nil {
		t.Fatal(err)
	}
	defer disk.Close()
	if err := s.cleanup(destination); err == nil {
		t.Fatal("cleaned up a running original")
	}
	if _, err := os.Stat(filepath.Join(source, "settings.json")); err != nil {
		t.Fatal("partially removed running original")
	}
}

func TestMoveRejectsOtherInstallDefaultAndOccupiedDestination(t *testing.T) {
	s, source, destination := moveFixture(t)
	s.defaultDir = filepath.Join(filepath.Dir(source), "default")
	if err := saveDataLocationPointer(s.defaultDir, filepath.Join(filepath.Dir(source), "another-install")); err != nil {
		t.Fatal(err)
	}
	if _, err := s.prepare(source, s.defaultDir, nil); err == nil {
		t.Fatal("replaced another installation's default")
	}
	if err := os.MkdirAll(destination, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(destination, "foreign"), []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.prepare(source, destination, nil); err == nil {
		t.Fatal("accepted occupied destination")
	}
	if _, err := s.prepare(source, filepath.Join(source, "nested"), nil); err == nil {
		t.Fatal("accepted nested destination")
	}
}

func TestMoveCancellationDiscardsOnlyStaging(t *testing.T) {
	s, source, destination := moveFixture(t)
	os.MkdirAll(filepath.Dir(destination), 0700)
	_, err := s.prepare(source, destination, func(int64, int64, string) {
		state, e := s.load()
		if e == nil && state.Pending != nil {
			requestSetupCancel()
		}
	})
	if !errors.Is(err, errSetupCancelled) {
		t.Fatalf("cancel: %v", err)
	}
	configureSetupCancellation(false)
	state, err := s.load()
	if err != nil || state.Pending == nil {
		t.Fatalf("missing recovery record: %v", err)
	}
	stage := state.Pending.Stage
	if err := s.recover(func(*installationMove) error { t.Fatal("activated cancelled move"); return nil }); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(stage); !os.IsNotExist(err) {
		t.Fatalf("staging remains: %v", err)
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(destination); !os.IsNotExist(err) {
		t.Fatal("cancelled move published")
	}
}

func TestMoveActivationFailureRetriesWithoutReverting(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	err := s.recover(func(*installationMove) error {
		// Simulate a shortcut update followed by a registry failure.
		os.WriteFile(filepath.Join(destination, "settings.json"), []byte("activation metadata"), 0600)
		return errors.New("registration failed")
	})
	if err == nil {
		t.Fatal("ignored activation failure")
	}
	state, err := s.load()
	if err != nil || state.Pending == nil || state.Pending.Phase != "activating" {
		t.Fatalf("lost activation journal: %v", err)
	}
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, _ = s.load()
	if state.Pending != nil || state.Retained == nil {
		t.Fatal("activation did not finish")
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); err != nil {
		t.Fatal("activation removed original")
	}
}

func TestMoveRecoveryRejectsCorruptedPublishedCopy(t *testing.T) {
	s, source, destination := moveFixture(t)
	m := prepareFixtureMove(t, s, source, destination)
	if err := os.Rename(m.Stage, destination); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(destination, "settings.json"), []byte("corrupt"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := s.recover(func(*installationMove) error { t.Fatal("activated corrupt copy"); return nil }); err == nil {
		t.Fatal("accepted corrupted copy")
	}
}

func TestMoveCleanupRejectsChangedOriginalAndResumes(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	s.markBooted(destination)
	path := filepath.Join(source, "settings.json")
	original, _ := os.ReadFile(path)
	os.WriteFile(path, []byte("changed after move"), 0600)
	if err := s.cleanup(destination); err == nil {
		t.Fatal("deleted changed original")
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); err != nil {
		t.Fatal("partial cleanup despite changed file")
	}
	os.WriteFile(path, original, 0600)
	state, _ := s.load()
	state.Retained.Phase = "cleaning"
	s.save(state)
	os.Remove(path) // process exited after removing one inventoried file
	if err := s.cleanup(destination); err != nil {
		t.Fatal(err)
	}
}

func TestMoveRejectsLockedDiskPendingUpdateAndLowSpace(t *testing.T) {
	s, source, destination := moveFixture(t)
	os.MkdirAll(filepath.Dir(destination), 0700)
	disk, err := openBackupDisk(filepath.Join(source, "vm", "disk.raw"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.prepare(source, destination, nil); err == nil {
		t.Fatal("moved locked disk")
	}
	disk.Close()
	os.WriteFile(filepath.Join(source, updateStateFilename), []byte("pending"), 0600)
	if _, err := s.prepare(source, destination, nil); err == nil {
		t.Fatal("moved pending update")
	}
	os.Remove(filepath.Join(source, updateStateFilename))
	original := diskFreeBytes
	diskFreeBytes = func(string) (int64, error) { return 1, nil }
	defer func() { diskFreeBytes = original }()
	if _, err := s.prepare(source, destination, nil); !errors.Is(err, errInsufficientDiskSpace) {
		t.Fatalf("low space: %v", err)
	}
}

func TestMoveMutationLockExcludesOtherSettings(t *testing.T) {
	s, _, _ := moveFixture(t)
	first, err := lockMoveStore(s)
	if err != nil {
		t.Fatal(err)
	}
	if second, err := lockMoveStore(s); err == nil {
		second.Close()
		t.Fatal("concurrent mutation allowed")
	}
	first.Close()
	second, err := lockMoveStore(s)
	if err != nil {
		t.Fatal(err)
	}
	second.Close()
}

func TestMoveRejectsLinksAndRetainsUnexpectedStagingFiles(t *testing.T) {
	s, source, destination := moveFixture(t)
	link := filepath.Join(source, "linked-notes")
	if err := os.Symlink(filepath.Join(source, "settings.json"), link); err == nil {
		os.MkdirAll(filepath.Dir(destination), 0700)
		if _, err := s.prepare(source, destination, nil); err == nil {
			t.Fatal("copied link")
		}
		os.Remove(link)
	}
	m := prepareFixtureMove(t, s, source, destination)
	state, _ := s.load()
	state.Pending.Phase = "copying"
	s.save(state)
	foreign := filepath.Join(m.Stage, "unexpected.txt")
	os.WriteFile(foreign, []byte("keep"), 0600)
	if err := s.recover(func(*installationMove) error { return nil }); err == nil {
		t.Fatal("removed unexpected staging content")
	}
	if data, err := os.ReadFile(foreign); err != nil || string(data) != "keep" {
		t.Fatal("lost unexpected file")
	}
}

func TestMoveVolumeRecoveryRepairsOnlySelectedInstallation(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, _ := s.load()
	location := moveVolumeLocation{Volume: `\\?\Volume{12345678-1234-1234-1234-123456789abc}\`, Relative: "TryOmarchy"}
	state.Locations = map[string]moveVolumeLocation{destination: location}
	otherSource, otherTarget := filepath.Join(t.TempDir(), "other-source"), filepath.Join(t.TempDir(), "other-target")
	state.Redirects[otherSource] = otherTarget
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	relocated := filepath.Join(t.TempDir(), "TryOmarchy")
	if err := os.Rename(destination, relocated); err != nil {
		t.Fatal(err)
	}
	original := locateMoveVolume
	t.Cleanup(func() { locateMoveVolume = original })
	locateMoveVolume = func(got moveVolumeLocation) (string, error) {
		if got != location {
			t.Fatalf("wrong volume: %+v", got)
		}
		return relocated, nil
	}
	for _, requested := range []string{source, destination} {
		got, err := resolveMovedDirectory(state, requested)
		if err != nil || got != relocated {
			t.Fatalf("resolve %s = %q %v", requested, got, err)
		}
	}
	calls := 0
	repaired, err := s.relocate(state, source, func(m *installationMove) error {
		calls++
		if m.Source != destination || m.Destination != relocated {
			t.Fatalf("wrong repair: %+v", m)
		}
		return nil
	})
	if err != nil || calls != 1 {
		t.Fatalf("repair: calls=%d err=%v", calls, err)
	}
	if repaired.Redirects[source] != relocated || repaired.Redirects[destination] != relocated || repaired.Redirects[otherSource] != otherTarget {
		t.Fatal("incorrect redirects", repaired.Redirects)
	}
	if repaired.Retained.Destination != relocated {
		t.Fatal("retained cleanup still points at old letter")
	}
	pointed, found, err := loadDataLocationPointer(s.defaultDir)
	if err != nil || !found || pointed != relocated {
		t.Fatalf("pointer = %q %v %v", pointed, found, err)
	}
	if _, err := s.load(); err != nil {
		t.Fatal("repaired journal is invalid", err)
	}
}

func TestMoveVolumeRecoveryRefusesWrongMissingAndLinkedVolumes(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, _ := s.load()
	state.Locations = map[string]moveVolumeLocation{destination: {Volume: `\\?\Volume{12345678-1234-1234-1234-123456789abc}\`, Relative: "TryOmarchy"}}
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	original := locateMoveVolume
	t.Cleanup(func() { locateMoveVolume = original })
	unavailable := errors.New("volume identity does not match")
	locateMoveVolume = func(moveVolumeLocation) (string, error) { return "", unavailable }
	// Even an intact disk at the old letter cannot override the saved identity.
	if _, err := resolveMovedDirectory(state, source); !errors.Is(err, unavailable) {
		t.Fatalf("fell back to old letter: %v", err)
	}
	before, _ := os.ReadFile(filepath.Join(s.dir, moveStateName))
	if _, err := s.relocate(state, source, func(*installationMove) error { t.Fatal("activated wrong volume"); return nil }); !errors.Is(err, unavailable) {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(filepath.Join(s.dir, moveStateName))
	if !bytes.Equal(before, after) {
		t.Fatal("changed history on refusal")
	}
	locateMoveVolume = func(moveVolumeLocation) (string, error) { return filepath.Join(t.TempDir(), "missing"), nil }
	if _, err := resolveMovedDirectory(state, source); err == nil {
		t.Fatal("accepted missing disk")
	}
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(destination, link); err == nil {
		locateMoveVolume = func(moveVolumeLocation) (string, error) { return link, nil }
		if _, err := resolveMovedDirectory(state, source); err == nil {
			t.Fatal("accepted linked recovered target")
		}
	}
	locateMoveVolume = func(moveVolumeLocation) (string, error) { return destination, nil }
	failure := errors.New("shortcut repair failed")
	// Force a real relocated path, so entrypoint repair is required.
	relocated := filepath.Join(t.TempDir(), "TryOmarchy")
	if err := os.Rename(destination, relocated); err != nil {
		t.Fatal(err)
	}
	locateMoveVolume = func(moveVolumeLocation) (string, error) { return relocated, nil }
	if _, err := s.relocate(state, source, func(*installationMove) error { return failure }); !errors.Is(err, failure) {
		t.Fatal(err)
	}
	after, _ = os.ReadFile(filepath.Join(s.dir, moveStateName))
	if !bytes.Equal(before, after) {
		t.Fatal("lost retry history on repair failure")
	}
}

func TestMoveHostPruningPreservesOtherInstallations(t *testing.T) {
	s, source, destination := moveFixture(t)
	otherSource, otherTarget := filepath.Join(t.TempDir(), "source"), filepath.Join(t.TempDir(), "target")
	if err := s.save(moveState{Version: 1, Redirects: map[string]string{source: destination, otherSource: otherTarget}}); err != nil {
		t.Fatal(err)
	}
	if err := s.forgetInstallation(destination); err != nil {
		t.Fatal(err)
	}
	if err := s.pruneEmpty(); err != nil {
		t.Fatal(err)
	}
	state, err := s.load()
	if err != nil || state.Redirects[otherSource] != otherTarget {
		t.Fatal("lost other install", err)
	}
	if err := s.forgetInstallation(otherTarget); err != nil {
		t.Fatal(err)
	}
	if err := s.pruneEmpty(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(s.dir); !os.IsNotExist(err) {
		t.Fatalf("empty host store remains: %v", err)
	}
}

func TestMoveCleanupRefusesUnownedDirectory(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "notes.txt")
	if err := os.WriteFile(path, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	entry := moveFile{Name: "notes.txt", Size: 4, SHA256: testSHA256([]byte("keep"))}
	for _, verify := range []bool{false, true} {
		if err := removeMoveInventory(dir, []moveFile{entry}, verify); err == nil {
			t.Fatal("removed unowned directory")
		}
		if data, err := os.ReadFile(path); err != nil || string(data) != "keep" {
			t.Fatal("lost unrelated data")
		}
	}
}

func TestMoveRecordsReachableLegacyVolume(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, _ := s.load()
	state.Locations = nil
	state.Retained.Volume = nil
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	original := identifyMoveVolume
	t.Cleanup(func() { identifyMoveVolume = original })
	location := moveVolumeLocation{Volume: `\\?\Volume{12345678-1234-1234-1234-123456789abc}\`, Relative: "TryOmarchy"}
	identifyMoveVolume = func(path string) (*moveVolumeLocation, error) {
		if path != destination {
			t.Fatal("recorded wrong installation", path)
		}
		return &location, nil
	}
	updated, err := s.rememberVolume(state, source)
	if err != nil || updated.Locations[destination] != location {
		t.Fatalf("upgrade=%+v err=%v", updated.Locations, err)
	}
	if updated.Retained.Volume == nil || *updated.Retained.Volume != location {
		t.Fatal("retained history not upgraded")
	}
	if _, err := s.load(); err != nil {
		t.Fatal(err)
	}
}

func TestMovedUninstallReceiptAllowsOnlyRemovalRetry(t *testing.T) {
	s, source, destination := moveFixture(t)
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	state, _ := s.load()
	if err := saveUninstallState(destination); err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(filepath.Join(destination, "vm")); err != nil {
		t.Fatal(err)
	}
	if _, err := resolveMovedDirectory(state, source); err == nil {
		t.Fatal("normal launch accepted half-removed disk")
	}
	got, err := resolveMovedDirectory(state, source, true)
	if err != nil || got != destination {
		t.Fatalf("uninstall retry=%q err=%v", got, err)
	}
}

func TestInterruptedMoveRebasesGuardedStagingAfterVolumeChange(t *testing.T) {
	s, source, destination := moveFixture(t)
	m := prepareFixtureMove(t, s, source, destination)
	state, _ := s.load()
	state.Pending.Phase = "copying"
	state.Pending.Volume = &moveVolumeLocation{Volume: `\\?\Volume{12345678-1234-1234-1234-123456789abc}\`, Relative: "TryOmarchy"}
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	newDestination := filepath.Join(t.TempDir(), "TryOmarchy")
	newStage := filepath.Join(filepath.Dir(newDestination), ".TryOmarchy-move-"+m.ID)
	if err := os.Rename(m.Stage, newStage); err != nil {
		t.Fatal(err)
	}
	original := locateMoveVolume
	t.Cleanup(func() { locateMoveVolume = original })
	locateMoveVolume = func(moveVolumeLocation) (string, error) { return newDestination, nil }
	if err := s.relocatePending(state); err != nil {
		t.Fatal(err)
	}
	if err := validateUninstallDirectory(newStage); err != nil {
		t.Fatal("lost private cleanup guard", err)
	}
	if err := s.recover(func(*installationMove) error { t.Fatal("activated interrupted move"); return nil }); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(newStage); !os.IsNotExist(err) {
		t.Fatalf("staging remains: %v", err)
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); err != nil {
		t.Fatal("lost source", err)
	}
}
