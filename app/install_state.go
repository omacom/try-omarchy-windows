package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const (
	installReceiptVersion  = 1
	installReceiptFilename = "install-state.json"
	maxInstallReceiptBytes = 1 << 20
)

type verifiedArtifact struct {
	SHA256          string `json:"sha256"`
	Size            int64  `json:"size"`
	ModTimeUnixNano int64  `json:"modTimeUnixNano"`
}

type installReceipt struct {
	Version        int                         `json:"version"`
	Release        string                      `json:"release"`
	ManifestSHA256 string                      `json:"manifestSHA256"`
	Files          map[string]verifiedArtifact `json:"files"`
}

func normalizedRelease(release string) string {
	return strings.TrimRight(strings.TrimSpace(release), "/")
}

func normalizedSHA256(value string) string {
	return strings.ToLower(strings.TrimSpace(value))
}

// installReceiptMatches is the fast path for an already verified install.
// The receipt is bound to the trusted manifest and invalidated whenever a
// required file's size or modification time changes.
func installReceiptMatches(dir, release, manifestSHA256 string, names []string) (bool, error) {
	path := filepath.Join(dir, installReceiptFilename)
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if len(data) > maxInstallReceiptBytes {
		return false, nil
	}
	var receipt installReceipt
	if json.Unmarshal(data, &receipt) != nil ||
		receipt.Version != installReceiptVersion ||
		receipt.Release != normalizedRelease(release) ||
		receipt.ManifestSHA256 != normalizedSHA256(manifestSHA256) {
		return false, nil
	}
	for _, name := range names {
		entry, ok := receipt.Files[name]
		if !ok || !validSHA256(entry.SHA256) {
			return false, nil
		}
		info, err := os.Lstat(filepath.Join(dir, name))
		if os.IsNotExist(err) {
			return false, nil
		}
		if err != nil {
			return false, err
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size ||
			info.ModTime().UnixNano() != entry.ModTimeUnixNano {
			return false, nil
		}
	}
	return true, nil
}

// writeInstallReceipt records metadata only after every required file has
// been authenticated. Writing through a sibling temp file makes interruption
// leave either the old complete receipt or no usable new receipt.
func writeInstallReceipt(dir, release, manifestSHA256 string, names []string, sums map[string]string) error {
	receipt := installReceipt{
		Version:        installReceiptVersion,
		Release:        normalizedRelease(release),
		ManifestSHA256: normalizedSHA256(manifestSHA256),
		Files:          make(map[string]verifiedArtifact, len(names)),
	}
	if !validSHA256(receipt.ManifestSHA256) {
		return fmt.Errorf("manifest digest is not a valid SHA256")
	}
	for _, name := range names {
		if filepath.Base(name) != name || name == "." {
			return fmt.Errorf("invalid receipt filename %q", name)
		}
		want := normalizedSHA256(sums[name])
		if !validSHA256(want) {
			return fmt.Errorf("release manifest has no valid SHA256 for %s", name)
		}
		info, err := os.Lstat(filepath.Join(dir, name))
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("%s is not a regular file", name)
		}
		receipt.Files[name] = verifiedArtifact{
			SHA256:          want,
			Size:            info.Size(),
			ModTimeUnixNano: info.ModTime().UnixNano(),
		}
	}
	data, err := json.MarshalIndent(receipt, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := filepath.Join(dir, installReceiptFilename)
	tmp := path + ".part"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			f.Close()
			os.Remove(tmp)
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
	if err := os.Rename(tmp, path); err != nil {
		return err
	}
	ok = true
	return nil
}

func invalidateInstallReceipt(dir string) error {
	for _, suffix := range []string{"", ".part"} {
		err := os.Remove(filepath.Join(dir, installReceiptFilename) + suffix)
		if err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

// verifyFileSHA256 authenticates a regular file and reports progress while it
// hashes. A file changed during the read is rejected even if its digest happened
// to match the expected value observed at the start.
func verifyFileSHA256(path, want string, progress func(done, total int64)) (bool, error) {
	want = normalizedSHA256(want)
	if !validSHA256(want) {
		return false, fmt.Errorf("expected digest is not a valid SHA256")
	}
	before, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if !before.Mode().IsRegular() {
		return false, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return false, err
	}
	defer f.Close()
	h := sha256.New()
	buf := make([]byte, 1<<20)
	var done int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return false, err
		}
		n, readErr := f.Read(buf)
		if n > 0 {
			if _, err := h.Write(buf[:n]); err != nil {
				return false, err
			}
			done += int64(n)
			if progress != nil {
				progress(done, before.Size())
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return false, readErr
		}
	}
	after, err := f.Stat()
	if err != nil {
		return false, err
	}
	if done != before.Size() || after.Size() != before.Size() ||
		after.ModTime().UnixNano() != before.ModTime().UnixNano() {
		return false, nil
	}
	return hex.EncodeToString(h.Sum(nil)) == want, nil
}
