package main

import (
	"archive/zip"
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func backupFixture(t *testing.T) (string, string) {
	t.Helper()
	configureSetupCancellation(false)
	root := t.TempDir()
	dir := filepath.Join(root, "original")
	for _, name := range []string{"vm/disk.raw", "guest/build-spec.json", "guest/rootfs.ext4", "guest/vmlinuz-linux", "guest/initramfs-linux.img", "runtime/bin/qemu.exe", "settings.json", storageSettingsFilename} {
		target := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(target), 0700); err != nil {
			t.Fatal(err)
		}
		data := []byte("fixture " + name)
		if name == "vm/disk.raw" {
			data = append(data, make([]byte, 2<<20)...)
		}
		if err := os.WriteFile(target, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	return dir, filepath.Join(root, "backup.zip")
}

func TestVMBackupRoundTripPreservesOriginal(t *testing.T) {
	dir, archive := backupFixture(t)
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	destination := filepath.Join(filepath.Dir(dir), "restored")
	if err := restoreVMBackup(archive, destination); err != nil {
		t.Fatal(err)
	}
	err := filepath.WalkDir(dir, func(name string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(dir, name)
		want, _ := os.ReadFile(name)
		got, e := os.ReadFile(filepath.Join(destination, rel))
		if e != nil {
			return e
		}
		if !bytes.Equal(got, want) {
			t.Errorf("restored %s differs", rel)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := restoreVMBackup(archive, dir); err == nil {
		t.Fatal("replaced existing installation")
	}
	original, _ := os.ReadFile(archive)
	if err := writeVMBackup(dir, archive); err == nil {
		t.Fatal("replaced existing backup")
	}
	after, _ := os.ReadFile(archive)
	if !bytes.Equal(original, after) {
		t.Fatal("existing backup changed")
	}
}

func TestVMBackupRejectsLockedDiskAndPendingUpdate(t *testing.T) {
	dir, archive := backupFixture(t)
	lock, err := openBackupDisk(filepath.Join(dir, "vm", "disk.raw"))
	if err != nil {
		t.Fatal(err)
	}
	if err := writeVMBackup(dir, archive); err == nil {
		lock.Close()
		t.Fatal("backed up locked disk")
	}
	lock.Close()
	if err := os.WriteFile(filepath.Join(dir, payloadUpdateStateFilename), []byte("pending"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writeVMBackup(dir, archive); err == nil {
		t.Fatal("backed up pending update")
	}
	if _, err := os.Stat(archive); !os.IsNotExist(err) {
		t.Fatal("published failed backup")
	}
}

func TestVMBackupSpaceAndCancellationLeaveNoOutput(t *testing.T) {
	dir, archive := backupFixture(t)
	previous := diskFreeBytes
	t.Cleanup(func() { diskFreeBytes = previous; configureSetupCancellation(false) })
	diskFreeBytes = func(string) (int64, error) { return 0, nil }
	if err := writeVMBackup(dir, archive); !errors.Is(err, errInsufficientDiskSpace) {
		t.Fatalf("space failure: %v", err)
	}
	diskFreeBytes = previous
	requestSetupCancel()
	if err := writeVMBackup(dir, archive); !errors.Is(err, errSetupCancelled) {
		t.Fatalf("cancel failure: %v", err)
	}
	if _, err := os.Stat(archive); !os.IsNotExist(err) {
		t.Fatal("published cancelled backup")
	}
	matches, _ := filepath.Glob(filepath.Join(filepath.Dir(dir), ".try-omarchy-*"))
	if len(matches) != 0 {
		t.Fatalf("left temporary files: %v", matches)
	}
}

func rewriteBackup(t *testing.T, source, destination string, mutate func(string, []byte) (string, []byte)) {
	t.Helper()
	r, err := zip.OpenReader(source)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	out, err := os.Create(destination)
	if err != nil {
		t.Fatal(err)
	}
	w := zip.NewWriter(out)
	for _, f := range r.File {
		in, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		data, err := io.ReadAll(in)
		in.Close()
		if err != nil {
			t.Fatal(err)
		}
		name, data := mutate(f.Name, data)
		entry, err := w.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = entry.Write(data); err != nil {
			t.Fatal(err)
		}
	}
	if err = w.Close(); err != nil {
		t.Fatal(err)
	}
	if err = out.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestVMRestoreRejectsDamagedBackupWithoutPublishing(t *testing.T) {
	dir, archive := backupFixture(t)
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	damaged := filepath.Join(filepath.Dir(dir), "damaged.zip")
	rewriteBackup(t, archive, damaged, func(name string, data []byte) (string, []byte) {
		if name == "vm/disk.raw" {
			data[0] ^= 1
		}
		return name, data
	})
	destination := filepath.Join(filepath.Dir(dir), "restored")
	if err := restoreVMBackup(damaged, destination); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatalf("expected checksum failure, got %v", err)
	}
	if _, err := os.Stat(destination); !os.IsNotExist(err) {
		t.Fatal("published damaged restore")
	}
	matches, _ := filepath.Glob(filepath.Join(filepath.Dir(dir), ".try-omarchy-restore-*"))
	if len(matches) != 0 {
		t.Fatal("left failed staging directory")
	}
}

func TestVMRestoreSpaceAndCancellation(t *testing.T) {
	dir, archive := backupFixture(t)
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	destination := filepath.Join(filepath.Dir(dir), "restored")
	previous := diskFreeBytes
	t.Cleanup(func() { diskFreeBytes = previous; configureSetupCancellation(false) })
	diskFreeBytes = func(string) (int64, error) { return 0, nil }
	if err := restoreVMBackup(archive, destination); !errors.Is(err, errInsufficientDiskSpace) {
		t.Fatalf("space failure: %v", err)
	}
	diskFreeBytes = previous
	requestSetupCancel()
	if err := restoreVMBackup(archive, destination); !errors.Is(err, errSetupCancelled) {
		t.Fatalf("cancel failure: %v", err)
	}
	if _, err := os.Stat(destination); !os.IsNotExist(err) {
		t.Fatal("published failed restore")
	}
}

func TestVMBackupNamesAndDestination(t *testing.T) {
	for _, name := range []string{"../disk.raw", "guest/../settings.json", "guest/CON", "runtime/bin/file:stream", "guest/trailing.", "vm/disk.qcow2", "shared/personal.txt"} {
		if backupNameAllowed(name) {
			t.Errorf("accepted unsupported name %q", name)
		}
	}
	dir, _ := backupFixture(t)
	if err := writeVMBackup(dir, filepath.Join(dir, "guest", "backup.zip")); err == nil {
		t.Fatal("accepted backup inside data folder")
	}
}

func TestVMRestoreRejectsIncompleteAndUnsupportedArchives(t *testing.T) {
	dir, archive := backupFixture(t)
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"unsupported-version", "unexpected-file", "duplicate-case"} {
		t.Run(kind, func(t *testing.T) {
			changed := filepath.Join(filepath.Dir(dir), kind+".zip")
			rewriteBackup(t, archive, changed, func(name string, data []byte) (string, []byte) {
				if kind == "unsupported-version" && name == backupManifestName {
					data = bytes.Replace(data, []byte(`"version":1`), []byte(`"version":2`), 1)
				}
				if kind == "unexpected-file" && name == "vm/disk.raw" {
					name = "other/disk.raw"
				}
				if kind == "duplicate-case" && name == storageSettingsFilename {
					name = "SETTINGS.JSON"
				}
				return name, data
			})
			destination := filepath.Join(filepath.Dir(dir), kind)
			if err := restoreVMBackup(changed, destination); err == nil {
				t.Fatal("accepted invalid archive")
			}
			if _, err := os.Stat(destination); !os.IsNotExist(err) {
				t.Fatal("published invalid archive")
			}
		})
	}
	if err := os.Remove(filepath.Join(dir, "guest", "vmlinuz-linux")); err != nil {
		t.Fatal(err)
	}
	if err := writeVMBackup(dir, archive+".incomplete"); err == nil {
		t.Fatal("backed up missing kernel")
	}
}

func TestBackupAndRestoreReportCompleteProgress(t *testing.T) {
	dir, archive := backupFixture(t)
	check := func(run func(backupProgress) error) {
		var last, total int64
		err := run(func(current, maximum int64, name string) {
			if current < last || current > maximum || name == "" {
				t.Errorf("invalid progress: %d/%d for %q", current, maximum, name)
			}
			last, total = current, maximum
		})
		if err != nil {
			t.Fatal(err)
		}
		if total == 0 || last != total {
			t.Fatalf("incomplete progress: %d/%d", last, total)
		}
	}
	check(func(report backupProgress) error { return writeVMBackupProgress(dir, archive, report) })
	check(func(report backupProgress) error {
		return restoreVMBackupProgress(archive, filepath.Join(filepath.Dir(dir), "progress-restore"), report)
	})
}

func TestVMRestoreBudgetsCompressedSizeAndRestampsReceipts(t *testing.T) {
	dir, archive := backupFixture(t)
	spec := filepath.Join(dir, "guest", "build-spec.json")
	info, err := os.Stat(spec)
	if err != nil {
		t.Fatal(err)
	}
	stamp := int64(1700000000123456700)
	sum := strings.Repeat("ab", 32)
	receipt := `{"version":1,"release":"r","manifestSHA256":"` + sum + `","files":{"build-spec.json":{"sha256":"` + sum + `","size":` + strconv.FormatInt(info.Size(), 10) + `,"modTimeUnixNano":` + strconv.FormatInt(stamp, 10) + `}}}`
	if err := os.WriteFile(filepath.Join(dir, "guest", installReceiptFilename), []byte(receipt), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	z, err := zip.OpenReader(archive)
	if err != nil {
		t.Fatal(err)
	}
	manifest, files, err := readVMBackup(z)
	z.Close()
	if err != nil {
		t.Fatal(err)
	}
	var nominal int64
	for _, entry := range manifest.Files {
		nominal += entry.Size
	}
	estimate := restoreSpaceEstimate(manifest, files)
	if estimate >= nominal+diskSpaceReserve || estimate <= diskSpaceReserve {
		t.Fatalf("estimate %d should sit between the reserve and the nominal %d", estimate, nominal)
	}
	previous := diskFreeBytes
	t.Cleanup(func() { diskFreeBytes = previous })
	diskFreeBytes = func(string) (int64, error) { return estimate, nil }
	destination := filepath.Join(filepath.Dir(dir), "restored")
	if err := restoreVMBackup(archive, destination); err != nil {
		t.Fatalf("restore with compressed-size budget failed: %v", err)
	}
	restored, err := os.Stat(filepath.Join(destination, "guest", "build-spec.json"))
	if err != nil {
		t.Fatal(err)
	}
	if restored.ModTime().UnixNano() != stamp {
		t.Fatalf("receipt time not restored: got %d, want %d", restored.ModTime().UnixNano(), stamp)
	}
}
