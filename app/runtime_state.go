package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const runtimeReceiptFilename = "runtime-install-state.json"

type runtimeReceipt struct {
	Schema         int              `json:"schema"`
	Release        string           `json:"release"`
	ManifestSHA256 string           `json:"manifestSHA256"`
	ArchiveSHA256  string           `json:"archiveSHA256"`
	Executable     verifiedArtifact `json:"executable"`
}

func runtimeReceiptIdentity(root string) (string, string, bool) {
	data, err := os.ReadFile(filepath.Join(root, runtimeReceiptFilename))
	if err != nil || len(data) > maxInstallReceiptBytes {
		return "", "", false
	}
	var receipt runtimeReceipt
	if json.Unmarshal(data, &receipt) != nil || receipt.Schema != 1 ||
		receipt.Release == "" || !validSHA256(receipt.ManifestSHA256) {
		return "", "", false
	}
	return receipt.Release, receipt.ManifestSHA256, true
}

func runtimeReceiptMatches(root, release, manifestSHA, archiveSHA string) bool {
	data, err := os.ReadFile(filepath.Join(root, runtimeReceiptFilename))
	if err != nil || len(data) > maxInstallReceiptBytes {
		return false
	}
	var receipt runtimeReceipt
	if json.Unmarshal(data, &receipt) != nil || receipt.Schema != 1 ||
		!releaseLocationsEquivalent(receipt.Release, release) ||
		receipt.ManifestSHA256 != normalizedSHA256(manifestSHA) ||
		receipt.ArchiveSHA256 != normalizedSHA256(archiveSHA) ||
		!validSHA256(receipt.Executable.SHA256) {
		return false
	}
	info, err := os.Lstat(filepath.Join(root, "bin", "qemu-system-x86_64w.exe"))
	return err == nil && info.Mode().IsRegular() && info.Size() == receipt.Executable.Size &&
		info.ModTime().UnixNano() == receipt.Executable.ModTimeUnixNano
}

func writeRuntimeReceipt(root, release, manifestSHA, archiveSHA string) error {
	executable := filepath.Join(root, "bin", "qemu-system-x86_64w.exe")
	info, err := os.Lstat(executable)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("runtime executable is not a regular file")
	}
	f, err := os.Open(executable)
	if err != nil {
		return err
	}
	h := sha256.New()
	_, copyErr := io.Copy(h, f)
	closeErr := f.Close()
	if copyErr != nil {
		return copyErr
	}
	if closeErr != nil {
		return closeErr
	}
	receipt := runtimeReceipt{
		Schema: 1, Release: normalizedRelease(release),
		ManifestSHA256: normalizedSHA256(manifestSHA), ArchiveSHA256: normalizedSHA256(archiveSHA),
		Executable: verifiedArtifact{
			SHA256: hex.EncodeToString(h.Sum(nil)), Size: info.Size(), ModTimeUnixNano: info.ModTime().UnixNano(),
		},
	}
	if !validSHA256(receipt.ManifestSHA256) || !validSHA256(receipt.ArchiveSHA256) {
		return fmt.Errorf("runtime receipt trust root is invalid")
	}
	data, err := json.MarshalIndent(receipt, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := filepath.Join(root, runtimeReceiptFilename)
	return os.WriteFile(path, data, 0o644)
}
