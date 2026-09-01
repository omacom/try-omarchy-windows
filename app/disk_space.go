package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const (
	diskSpaceReserve      int64 = 1 << 30
	maxGuestManifestBytes       = 1 << 20
	maxGuestArtifactBytes int64 = 1 << 40
)

var errInsufficientDiskSpace = errors.New("not enough free disk space")

var (
	diskFreeBytes      = platformDiskFreeBytes
	allocatedFileBytes = platformAllocatedFileBytes
)

type guestArtifactSizes struct {
	SchemaVersion int `json:"schemaVersion"`
	Artifacts     []struct {
		Bytes  int64  `json:"bytes"`
		Path   string `json:"path"`
		SHA256 string `json:"sha256"`
	} `json:"artifacts"`
}

func readGuestArtifactSizes(path string, sums map[string]string) (map[string]int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, maxGuestManifestBytes+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxGuestManifestBytes {
		return nil, fmt.Errorf("guest artifact manifest is too large")
	}
	var manifest guestArtifactSizes
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("parsing guest artifact manifest: %w", err)
	}
	if manifest.SchemaVersion != 1 {
		return nil, fmt.Errorf("unsupported guest artifact manifest schema %d", manifest.SchemaVersion)
	}
	sizes := make(map[string]int64, len(manifest.Artifacts))
	for _, artifact := range manifest.Artifacts {
		if filepath.Base(artifact.Path) != artifact.Path || artifact.Path == "." {
			return nil, fmt.Errorf("invalid guest artifact path %q", artifact.Path)
		}
		if artifact.Bytes <= 0 || artifact.Bytes > maxGuestArtifactBytes {
			return nil, fmt.Errorf("invalid size for guest artifact %s", artifact.Path)
		}
		if _, exists := sizes[artifact.Path]; exists {
			return nil, fmt.Errorf("duplicate guest artifact %s", artifact.Path)
		}
		if !validSHA256(artifact.SHA256) || normalizedSHA256(artifact.SHA256) != normalizedSHA256(sums[artifact.Path]) {
			return nil, fmt.Errorf("guest artifact metadata does not match authenticated SHA256SUMS for %s", artifact.Path)
		}
		sizes[artifact.Path] = artifact.Bytes
	}
	for _, name := range []string{"rootfs.ext4", "rootfs.ext4.zst"} {
		if sizes[name] == 0 {
			return nil, fmt.Errorf("guest artifact manifest has no size for %s", name)
		}
	}
	return sizes, nil
}

func remainingFileBytes(path string, total int64) int64 {
	var existing int64
	for _, candidate := range []string{path, path + ".part"} {
		info, err := os.Lstat(candidate)
		if err == nil && info.Mode().IsRegular() && info.Size() > existing {
			existing = info.Size()
		}
	}
	if existing >= total {
		return 0
	}
	return total - existing
}

func guestInstallSpaceRequired(archiveRemaining, rootfsAllocatedBytes int64) (int64, error) {
	if archiveRemaining < 0 || rootfsAllocatedBytes <= 0 ||
		archiveRemaining > (1<<63-1)-rootfsAllocatedBytes-diskSpaceReserve {
		return 0, fmt.Errorf("guest artifact sizes overflow")
	}
	return archiveRemaining + rootfsAllocatedBytes + diskSpaceReserve, nil
}

// estimatedSparseRootfsBytes budgets for the zero blocks decompress omits.
// The current 6.00 GiB image measures 3.87 GiB allocated; two thirds leaves
// some headroom, with diskSpaceReserve covering normal release-to-release drift.
func estimatedSparseRootfsBytes(logicalBytes int64) (int64, error) {
	if logicalBytes <= 0 || logicalBytes > maxGuestArtifactBytes {
		return 0, fmt.Errorf("invalid logical rootfs size")
	}
	return logicalBytes - logicalBytes/3, nil
}

func requireDiskSpace(path string, required int64) error {
	if required <= 0 {
		return nil
	}
	available, err := diskFreeBytes(path)
	if err != nil {
		return fmt.Errorf("checking free disk space: %w", err)
	}
	if available < required {
		return fmt.Errorf("%w in %s: need %s available, have %s",
			errInsufficientDiskSpace, filepath.Clean(path), formatGiB(required), formatGiB(available))
	}
	return nil
}

func formatGiB(bytes int64) string {
	return fmt.Sprintf("%.1f GiB", float64(bytes)/float64(int64(1)<<30))
}
