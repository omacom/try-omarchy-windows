package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func artifactSum(data string) string {
	sum := sha256.Sum256([]byte(data))
	return hex.EncodeToString(sum[:])
}

func writeGuestSizeManifest(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "guest-manifest.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestReadGuestArtifactSizes(t *testing.T) {
	rootSum := artifactSum("root")
	archiveSum := artifactSum("archive")
	path := writeGuestSizeManifest(t, `{"schemaVersion":1,"artifacts":[`+
		`{"path":"rootfs.ext4","bytes":6442450944,"sha256":"`+rootSum+`"},`+
		`{"path":"rootfs.ext4.zst","bytes":1445337714,"sha256":"`+archiveSum+`"}]}`)
	sizes, err := readGuestArtifactSizes(path, map[string]string{
		"rootfs.ext4": rootSum, "rootfs.ext4.zst": archiveSum,
	})
	if err != nil {
		t.Fatal(err)
	}
	if sizes["rootfs.ext4"] != 6442450944 || sizes["rootfs.ext4.zst"] != 1445337714 {
		t.Fatalf("sizes = %#v", sizes)
	}
}

func TestReadGuestArtifactSizesRejectsUnauthenticatedMetadata(t *testing.T) {
	rootSum := artifactSum("root")
	archiveSum := artifactSum("archive")
	path := writeGuestSizeManifest(t, `{"schemaVersion":1,"artifacts":[`+
		`{"path":"rootfs.ext4","bytes":6442450944,"sha256":"`+rootSum+`"},`+
		`{"path":"rootfs.ext4.zst","bytes":1445337714,"sha256":"`+archiveSum+`"}]}`)
	_, err := readGuestArtifactSizes(path, map[string]string{
		"rootfs.ext4": artifactSum("different"), "rootfs.ext4.zst": archiveSum,
	})
	if err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Fatalf("error = %v, want authenticated-metadata failure", err)
	}
}

func TestReadGuestArtifactSizesRejectsDuplicate(t *testing.T) {
	rootSum := artifactSum("root")
	archiveSum := artifactSum("archive")
	path := writeGuestSizeManifest(t, `{"schemaVersion":1,"artifacts":[`+
		`{"path":"rootfs.ext4","bytes":1,"sha256":"`+rootSum+`"},`+
		`{"path":"rootfs.ext4","bytes":2,"sha256":"`+rootSum+`"},`+
		`{"path":"rootfs.ext4.zst","bytes":1,"sha256":"`+archiveSum+`"}]}`)
	_, err := readGuestArtifactSizes(path, map[string]string{
		"rootfs.ext4": rootSum, "rootfs.ext4.zst": archiveSum,
	})
	if err == nil || !strings.Contains(err.Error(), "duplicate") {
		t.Fatalf("error = %v, want duplicate failure", err)
	}
}

func TestGuestInstallSpaceRequiredIncludesResumeAndReserve(t *testing.T) {
	required, err := guestInstallSpaceRequired(512<<20, 4<<30)
	if err != nil {
		t.Fatal(err)
	}
	if want := int64(512<<20) + (int64(4) << 30) + diskSpaceReserve; required != want {
		t.Fatalf("required = %d, want %d", required, want)
	}
}

func TestEstimatedSparseRootfsBytesUsesConservativeMeasuredRatio(t *testing.T) {
	got, err := estimatedSparseRootfsBytes(6 << 30)
	if err != nil {
		t.Fatal(err)
	}
	if want := int64(4) << 30; got != want {
		t.Fatalf("estimated sparse rootfs = %d, want %d", got, want)
	}
}

func TestSparseEstimateAllowsMeasuredSevenGiBInstall(t *testing.T) {
	rootfsAllocated, err := estimatedSparseRootfsBytes(6 << 30)
	if err != nil {
		t.Fatal(err)
	}
	required, err := guestInstallSpaceRequired(1445169669, rootfsAllocated)
	if err != nil {
		t.Fatal(err)
	}
	if required >= 7<<30 {
		t.Fatalf("required = %d, want less than 7 GiB", required)
	}
}

func TestEstimatedSparseRootfsBytesRejectsInvalidSize(t *testing.T) {
	if _, err := estimatedSparseRootfsBytes(0); err == nil {
		t.Fatal("zero logical rootfs size was accepted")
	}
}

func TestRemainingFileBytesUsesLargestStagedFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "payload")
	if err := os.WriteFile(path, make([]byte, 3), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path+".part", make([]byte, 7), 0o644); err != nil {
		t.Fatal(err)
	}
	if got := remainingFileBytes(path, 10); got != 3 {
		t.Fatalf("remaining = %d, want 3", got)
	}
}

func TestRequireDiskSpaceReportsRequiredAndAvailable(t *testing.T) {
	original := diskFreeBytes
	diskFreeBytes = func(string) (int64, error) { return 2 << 30, nil }
	t.Cleanup(func() { diskFreeBytes = original })
	err := requireDiskSpace(t.TempDir(), 3<<30)
	if err == nil || !strings.Contains(err.Error(), "need 3.0 GiB") || !strings.Contains(err.Error(), "have 2.0 GiB") {
		t.Fatalf("error = %v", err)
	}
	if !errors.Is(err, errInsufficientDiskSpace) {
		t.Fatalf("error = %v, want insufficient-disk-space sentinel", err)
	}
}

func TestRequireDiskSpacePropagatesProbeFailure(t *testing.T) {
	original := diskFreeBytes
	want := errors.New("probe failed")
	diskFreeBytes = func(string) (int64, error) { return 0, want }
	t.Cleanup(func() { diskFreeBytes = original })
	if err := requireDiskSpace(t.TempDir(), 1); !errors.Is(err, want) {
		t.Fatalf("error = %v, want %v", err, want)
	}
}
