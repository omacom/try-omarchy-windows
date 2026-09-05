package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"
)

const backupManifestName = "backup.json"
const backupMaxFiles = 10000
const backupMaxBytes int64 = 2 << 40

type backupEntry struct {
	Name   string `json:"name"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}
type backupManifest struct {
	Version int           `json:"version"`
	Files   []backupEntry `json:"files"`
}

func backupNameAllowed(name string) bool {
	if name != path.Clean(name) || strings.ContainsAny(name, "\\:") || strings.HasPrefix(name, "/") {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "." || part == ".." || strings.TrimRight(part, " .") != part {
			return false
		}
		base := strings.ToUpper(strings.SplitN(part, ".", 2)[0])
		if base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || (len(base) == 4 && (strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT")) && base[3] >= '0' && base[3] <= '9') {
			return false
		}
	}
	return name == "vm/disk.raw" || name == "settings.json" || name == storageSettingsFilename || strings.HasPrefix(name, "guest/") || strings.HasPrefix(name, "runtime/")
}

func requiredBackupFiles(files map[string]bool) error {
	for _, name := range []string{"vm/disk.raw", "guest/build-spec.json", "guest/rootfs.ext4", "guest/vmlinuz-linux", "guest/initramfs-linux.img"} {
		if !files[name] {
			return fmt.Errorf("backup is missing %s", name)
		}
	}
	return nil
}

// Backup and restore are called while the launcher holds its lifecycle lock.
// Holding the disk itself exclusively also catches an orphaned QEMU process.
func writeVMBackup(dir, destination string) error {
	return writeVMBackupProgress(dir, destination, nil)
}

func writeVMBackupProgress(dir, destination string, report backupProgress) error {
	root, err := filepath.EvalSymlinks(dir)
	if err != nil {
		return err
	}
	parent, err := filepath.EvalSymlinks(filepath.Dir(destination))
	if err != nil {
		return err
	}
	root, err = filepath.Abs(root)
	if err != nil {
		return err
	}
	parent, err = filepath.Abs(parent)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(root, parent)
	if err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return fmt.Errorf("save the backup outside the Try Omarchy data folder")
	}

	for _, name := range []string{payloadUpdateStateFilename, updateStateFilename} {
		if _, err := os.Lstat(filepath.Join(dir, name)); !os.IsNotExist(err) {
			return fmt.Errorf("finish the pending update before backing up")
		}
	}
	disk, err := openBackupDisk(filepath.Join(dir, "vm", "disk.raw"))
	if err != nil {
		return fmt.Errorf("close Try Omarchy before backing up: %w", err)
	}
	defer disk.Close()
	var entries []backupEntry
	seen := map[string]bool{}
	var total int64
	for _, root := range []string{"guest", "runtime", "vm/disk.raw", "settings.json", storageSettingsFilename} {
		full := filepath.Join(dir, filepath.FromSlash(root))
		if _, err := os.Lstat(full); os.IsNotExist(err) {
			continue
		}
		err := filepath.WalkDir(full, func(name string, d os.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if d.IsDir() {
				return nil
			}
			info, err := d.Info()
			if err != nil {
				return err
			}
			rel, err := filepath.Rel(dir, name)
			if err != nil {
				return err
			}
			rel = filepath.ToSlash(rel)
			if !info.Mode().IsRegular() || !backupNameAllowed(rel) {
				return fmt.Errorf("cannot back up unsupported file %s", rel)
			}
			if len(entries) >= backupMaxFiles || info.Size() > backupMaxBytes-total {
				return fmt.Errorf("backup exceeds supported size")
			}
			total += info.Size()
			entries = append(entries, backupEntry{Name: rel, Size: info.Size()})
			seen[rel] = true
			return nil
		})
		if err != nil {
			return err
		}
	}
	if err := requiredBackupFiles(seen); err != nil {
		return err
	}
	// Budget uncompressed size so success does not depend on compressibility.
	if err := requireDiskSpace(filepath.Dir(destination), total+diskSpaceReserve); err != nil {
		return err
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("choose a new backup filename")
	}
	f, err := os.CreateTemp(filepath.Dir(destination), ".try-omarchy-backup-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	zw := zip.NewWriter(f)
	var processed int64
	for i := range entries {
		entry := &entries[i]
		source := disk
		if entry.Name != "vm/disk.raw" {
			source, err = os.Open(filepath.Join(dir, filepath.FromSlash(entry.Name)))
			if err != nil {
				zw.Close()
				return err
			}
		}
		header := &zip.FileHeader{Name: entry.Name, Method: zip.Deflate}
		header.SetMode(0600)
		writer, e := zw.CreateHeader(header)
		h := sha256.New()
		var n int64
		if e == nil {
			n, e = io.Copy(io.MultiWriter(writer, h), setupReader{&backupProgressReader{source: io.LimitReader(source, entry.Size+1), processed: &processed, total: total, name: entry.Name, report: report}})
		}
		if source != disk {
			source.Close()
		}
		if e != nil {
			zw.Close()
			return e
		}
		if n != entry.Size {
			zw.Close()
			return fmt.Errorf("%s changed while backing up", entry.Name)
		}
		entry.SHA256 = hex.EncodeToString(h.Sum(nil))
	}
	writer, err := zw.Create(backupManifestName)
	if err != nil {
		zw.Close()
		return err
	}
	if err = json.NewEncoder(writer).Encode(backupManifest{Version: 1, Files: entries}); err != nil {
		zw.Close()
		return err
	}
	if err = zw.Close(); err != nil {
		return err
	}
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	// A hard link publishes without replacing a backup created concurrently.
	if err = os.Link(f.Name(), destination); err != nil {
		return fmt.Errorf("publishing backup (use an NTFS or ReFS destination): %w", err)
	}
	return nil
}

func readVMBackup(z *zip.ReadCloser) (backupManifest, map[string]*zip.File, error) {
	var manifest backupManifest
	files := map[string]*zip.File{}
	names := map[string]bool{}
	if len(z.File) > backupMaxFiles+1 {
		return manifest, nil, fmt.Errorf("too many backup files")
	}
	for _, f := range z.File {
		key := strings.ToLower(f.Name)
		if names[key] || !f.Mode().IsRegular() || (f.Name != backupManifestName && !backupNameAllowed(f.Name)) {
			return manifest, nil, fmt.Errorf("unsupported or duplicate backup file %q", f.Name)
		}
		names[key] = true
		files[f.Name] = f
	}
	metadata := files[backupManifestName]
	if metadata == nil || metadata.UncompressedSize64 > 4<<20 {
		return manifest, nil, fmt.Errorf("missing or oversized backup manifest")
	}
	r, err := metadata.Open()
	if err != nil {
		return manifest, nil, err
	}
	defer r.Close()
	dec := json.NewDecoder(io.LimitReader(r, (4<<20)+1))
	dec.DisallowUnknownFields()
	if err = dec.Decode(&manifest); err != nil {
		return manifest, nil, err
	}
	if dec.Decode(&struct{}{}) != io.EOF || manifest.Version != 1 || len(manifest.Files) != len(files)-1 {
		return manifest, nil, fmt.Errorf("unsupported backup manifest")
	}
	seen := map[string]bool{}
	var total int64
	for _, entry := range manifest.Files {
		f := files[entry.Name]
		if !backupNameAllowed(entry.Name) || seen[entry.Name] || f == nil || entry.Size < 0 || entry.Size > backupMaxBytes-total || uint64(entry.Size) != f.UncompressedSize64 || !validSHA256(entry.SHA256) {
			return manifest, nil, fmt.Errorf("invalid backup entry %q", entry.Name)
		}
		seen[entry.Name] = true
		total += entry.Size
	}
	return manifest, files, requiredBackupFiles(seen)
}

type backupSparseWriter struct{ file *os.File }

func (w backupSparseWriter) Write(p []byte) (int, error) {
	zero := true
	for _, b := range p {
		if b != 0 {
			zero = false
			break
		}
	}
	if zero {
		_, err := w.file.Seek(int64(len(p)), io.SeekCurrent)
		if err != nil {
			return 0, err
		}
		return len(p), nil
	}
	return w.file.Write(p)
}

// Restore only publishes into a new directory. Existing installations are never
// renamed or deleted, including when validation, copying, or publication fails.
func restoreVMBackup(source, destination string) error {
	return restoreVMBackupProgress(source, destination, nil)
}

func restoreVMBackupProgress(source, destination string, report backupProgress) error {
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("restore requires a new data folder; the existing folder was not changed")
	}
	z, err := zip.OpenReader(source)
	if err != nil {
		return err
	}
	defer z.Close()
	manifest, files, err := readVMBackup(z)
	if err != nil {
		return err
	}
	var total int64
	for _, entry := range manifest.Files {
		total += entry.Size
	}
	if err = requireDiskSpace(filepath.Dir(destination), total+diskSpaceReserve); err != nil {
		return err
	}
	staging, err := os.MkdirTemp(filepath.Dir(destination), ".try-omarchy-restore-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(staging)
	var processed int64
	for _, entry := range manifest.Files {
		target := filepath.Join(staging, filepath.FromSlash(entry.Name))
		if err = os.MkdirAll(filepath.Dir(target), 0700); err != nil {
			return err
		}
		if err = restoreBackupFile(files[entry.Name], target, entry, &processed, total, report); err != nil {
			return err
		}
	}
	// The main launcher uses Windows, where Rename cannot replace a directory.
	if _, err = os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("restore destination appeared during copying")
	}
	return os.Rename(staging, destination)
}

func restoreBackupFile(z *zip.File, target string, entry backupEntry, processed *int64, total int64, report backupProgress) error {
	r, err := z.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer f.Close()
	if err = setSparse(f); err != nil {
		return fmt.Errorf("restore needs sparse-file support: %w", err)
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(backupSparseWriter{f}, h), setupReader{&backupProgressReader{source: io.LimitReader(r, entry.Size+1), processed: processed, total: total, name: entry.Name, report: report}})
	if err != nil {
		return err
	}
	if n != entry.Size || hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
		return fmt.Errorf("backup checksum mismatch for %s", entry.Name)
	}
	if err = f.Truncate(n); err != nil {
		return err
	}
	if err = f.Sync(); err != nil {
		return err
	}
	return f.Close()
}

// Progress is optional so archive tests and noninteractive callers never create UI.
type backupProgress func(current, total int64, name string)
type backupProgressReader struct {
	source    io.Reader
	processed *int64
	total     int64
	name      string
	report    backupProgress
}

func (r *backupProgressReader) Read(p []byte) (int, error) {
	n, err := r.source.Read(p)
	*r.processed += int64(n)
	if r.report != nil && n > 0 {
		r.report(*r.processed, r.total, r.name)
	}
	return n, err
}
