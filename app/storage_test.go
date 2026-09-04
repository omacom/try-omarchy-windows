package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestStoragePreferencesRemainCompatibleWithOldSettings(t *testing.T) {
	root := t.TempDir()
	original := []byte(`{"schemaVersion":1,"memoryMiB":2048}`)
	if err := os.WriteFile(settingsPath(root), original, 0600); err != nil {
		t.Fatal(err)
	}
	defaults, err := loadStorageSettings(root)
	if err != nil || defaults.DiskGiB != 0 {
		t.Fatalf("defaults = %+v, %v", defaults, err)
	}
	for _, size := range []int{64, 128, 0} {
		if err := saveStorageSettings(root, size); err != nil {
			t.Fatal(err)
		}
		got, err := loadStorageSettings(root)
		if err != nil || got.DiskGiB != size {
			t.Fatalf("preferences = %+v, %v", got, err)
		}
		old, err := loadSettings(settingsPath(root))
		if err != nil || old.MemoryMiB != 2048 {
			t.Fatalf("old settings = %+v, %v", old, err)
		}
		data, err := os.ReadFile(settingsPath(root))
		if err != nil || string(data) != string(original) {
			t.Fatal("existing settings changed")
		}
	}
	if err := saveStorageSettings(root, -1); err == nil {
		t.Fatal("invalid capacity saved")
	}
	got, err := loadStorageSettings(root)
	if err != nil || got.DiskGiB != 0 {
		t.Fatal("invalid save changed the preference")
	}
}

func TestStoragePreferencesRejectCorruption(t *testing.T) {
	for _, data := range []string{
		`{`, `{"schemaVersion":2}`, `{"schemaVersion":1,"diskGiB":-1}`,
		`{"schemaVersion":1,"diskGiB":64} {}`, `{"schemaVersion":1,"unknown":1}`,
	} {
		root := t.TempDir()
		if err := os.WriteFile(filepath.Join(root, storageSettingsFilename), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadStorageSettings(root); err == nil {
			t.Fatalf("accepted %s", data)
		}
	}
}

func TestRequestedCapacityAndPortableGuard(t *testing.T) {
	for _, tc := range []struct {
		requested int
		portable  bool
		want      int64
		bad       bool
	}{
		{0, false, 24 * 1024, false}, {64, false, 64 * 1024, false},
		{24, false, 24 * 1024, false}, {1024, false, 1024 * 1024, false},
		{-1, false, 0, true}, {23, false, 0, true}, {1025, false, 0, true},
		{0, true, 24 * 1024, false}, {64, true, 0, true},
	} {
		got, err := requestedDiskMiB(24*1024, tc.requested, tc.portable)
		if (err != nil) != tc.bad || (!tc.bad && got != tc.want) {
			t.Fatalf("request %+v = %d, %v", tc, got, err)
		}
	}
	if got, err := requestedDiskMiB(32*1024, 24, false); err != nil || got != 32*1024 {
		t.Fatal("preference reduced the factory minimum")
	}
	for _, input := range []string{"-1", "23", "1025", "24.5", "no", "999999999999999999999"} {
		if _, err := parseDiskGiB(input); err == nil {
			t.Fatalf("accepted %q", input)
		}
	}
}

func TestCapacityGrowthPreservesDataAndNeverShrinks(t *testing.T) {
	root := t.TempDir()
	guest := filepath.Join(root, "guest")
	vm := filepath.Join(root, "vm")
	for _, path := range []string{guest, vm} {
		if err := os.Mkdir(path, 0700); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(guest, "rootfs.ext4"), []byte("factory"), 0600); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vm, "disk.raw")
	content := []byte("existing guest files")
	if err := os.WriteFile(disk, content, 0600); err != nil {
		t.Fatal(err)
	}
	cfg := &config{dir: root, guestDir: guest, vmDir: vm, disk: disk, diskGiB: 24}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	cfg.diskGiB = 0
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(disk)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	got := make([]byte, len(content))
	if _, err := f.Read(got); err != nil || string(got) != string(content) {
		t.Fatalf("data changed: %q, %v", got, err)
	}
	info, err := f.Stat()
	if err != nil || info.Size() != 24<<30 {
		t.Fatalf("disk shrank: %v, %v", info, err)
	}
}

func TestInvalidCapacityCannotResetDisk(t *testing.T) {
	for _, portable := range []bool{false, true} {
		root := t.TempDir()
		disk := filepath.Join(root, "disk")
		if err := os.WriteFile(disk, []byte("keep"), 0600); err != nil {
			t.Fatal(err)
		}
		size := -1
		if portable {
			size = 64
		}
		cfg := &config{dir: root, disk: disk, fresh: true, portable: portable, diskGiB: size}
		if err := prepareDisk(cfg, 24*1024); err == nil {
			t.Fatal("invalid request accepted")
		}
		data, err := os.ReadFile(disk)
		if err != nil || string(data) != "keep" {
			t.Fatal("invalid request deleted the disk")
		}
	}
}
