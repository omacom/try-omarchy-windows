//go:build windows

package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/klauspost/compress/zstd"
)

// First-run setup: the exe IS the stub - it downloads the guest image from the
// GitHub release on first launch (with a progress window), verifies it against
// the authenticated SHA256SUMS, and decompresses the rootfs.

var (
	downloadedGuestArtifacts = []string{"guest-manifest.json", "build-spec.json", "vmlinuz-linux", "initramfs-linux.img"}
	installedGuestArtifacts  = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4"}
)

func ensureGuest(cfg *config, release, sumsSHA256 string) error {
	ready, err := installReceiptMatches(cfg.guestDir, release, sumsSHA256, installedGuestArtifacts)
	if err != nil {
		return fmt.Errorf("reading verified install state: %w", err)
	}
	if ready {
		os.Remove(filepath.Join(cfg.guestDir, "rootfs.ext4.zst"))
		return nil
	}
	oldRelease, oldManifest, haveOldReceipt := installReceiptIdentity(cfg.guestDir)
	isReleaseUpdate := haveOldReceipt && (oldRelease != normalizedRelease(release) ||
		oldManifest != normalizedSHA256(sumsSHA256))
	if !isReleaseUpdate {
		return ensureGuestFiles(cfg, release, sumsSHA256)
	}

	ui := getUI()
	ui.setStatus("Preparing an Omarchy image update...")
	staged := filepath.Join(cfg.dir, "guest.next")
	if err := os.RemoveAll(staged); err != nil {
		return err
	}
	stagedCfg := *cfg
	stagedCfg.guestDir = staged
	if err := ensureGuestFiles(&stagedCfg, release, sumsSHA256); err != nil {
		_ = os.RemoveAll(staged)
		return err
	}
	if err := recordPayloadUpdate(cfg.dir, releaseVersion(release), true, false); err != nil {
		return fmt.Errorf("recording image rollback state: %w", err)
	}
	if err := publishDirectoryUpdate(cfg.guestDir, staged, filepath.Join(cfg.dir, "guest.previous")); err != nil {
		// Keep the rollback record. Publication may have moved the old tree
		// before failing, and the next launch is the safest place to reconcile it.
		return fmt.Errorf("publishing image update: %w", err)
	}
	return nil
}

func ensureGuestFiles(cfg *config, release, sumsSHA256 string) error {
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	if err := os.MkdirAll(cfg.guestDir, 0o755); err != nil {
		return err
	}
	ready, err := installReceiptMatches(cfg.guestDir, release, sumsSHA256, installedGuestArtifacts)
	if err != nil {
		return fmt.Errorf("reading verified install state: %w", err)
	}
	if ready {
		// A crash after the receipt commit but before cleanup can leave this
		// reconstructible archive behind. It is never needed for launch.
		os.Remove(filepath.Join(cfg.guestDir, "rootfs.ext4.zst"))
		return nil
	}
	if err := invalidateInstallReceipt(cfg.guestDir); err != nil {
		return fmt.Errorf("invalidating stale install state: %w", err)
	}

	ui := getUI()
	client := newDownloadClient()

	sums, err := releaseSumsForConfig(cfg, client, release, sumsSHA256)
	if err != nil {
		return fmt.Errorf("authenticating SHA256SUMS: %w", err)
	}
	for _, name := range []string{"guest-manifest.json", "build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4", "rootfs.ext4.zst"} {
		if !validSHA256(sums[name]) {
			return fmt.Errorf("release manifest has no valid SHA256 for %s", name)
		}
	}

	for i, name := range downloadedGuestArtifacts {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		dest := filepath.Join(cfg.guestDir, name)
		status := fmt.Sprintf("Downloading Omarchy (%d of %d)...", i+1, len(downloadedGuestArtifacts)+1)
		var installErr error
		if cfg.portable {
			status = fmt.Sprintf("Checking portable Omarchy (%d of %d)...", i+1, len(downloadedGuestArtifacts)+1)
			installErr = ensureVerifiedPortableCopy(filepath.Join(cfg.payloadDir, name), dest, sums[name], status, ui)
		} else {
			installErr = ensureVerifiedDownload(client, normalizedRelease(release)+"/"+name, dest, sums[name], status, ui)
		}
		if installErr != nil {
			return fmt.Errorf("preparing %s: %w", name, installErr)
		}
	}
	artifactSizes, err := readGuestArtifactSizes(filepath.Join(cfg.guestDir, "guest-manifest.json"), sums)
	if err != nil {
		return fmt.Errorf("reading authenticated guest artifact sizes: %w", err)
	}

	zst := filepath.Join(cfg.guestDir, "rootfs.ext4.zst")
	removeZst := true
	if cfg.portable {
		zst = filepath.Join(cfg.payloadDir, "rootfs.ext4.zst")
		removeZst = false
	}
	rootfs := filepath.Join(cfg.guestDir, "rootfs.ext4")
	if _, err := os.Lstat(rootfs); err == nil {
		ui.setStatus("Checking the cached Omarchy system...")
	}
	rootfsOK, err := verifyFileSHA256(rootfs, sums["rootfs.ext4"], ui.setProgress)
	if err != nil {
		return fmt.Errorf("verifying rootfs.ext4: %w", err)
	}
	if !rootfsOK {
		if err := removeCachedFile(rootfs); err != nil {
			return fmt.Errorf("removing incomplete rootfs.ext4: %w", err)
		}
		rootfsAllocated, err := rootfsInstallBytes(artifactSizes["rootfs.ext4"], cfg.portable)
		if err != nil {
			return err
		}
		required, err := guestInstallSpaceRequired(remainingFileBytes(zst, artifactSizes["rootfs.ext4.zst"]), rootfsAllocated)
		if err != nil {
			return err
		}
		if err := requireDiskSpace(cfg.guestDir, required); err != nil {
			return fmt.Errorf("preflighting Omarchy storage: %w", err)
		}
		if cfg.portable {
			ui.setStatus("Checking the portable Omarchy system...")
			ok, err := verifyFileSHA256(zst, sums["rootfs.ext4.zst"], ui.setProgress)
			if err != nil {
				return fmt.Errorf("checking rootfs.ext4.zst: %w", err)
			}
			if !ok {
				return fmt.Errorf("checksum mismatch for rootfs.ext4.zst")
			}
		} else if err := ensureVerifiedDownload(client, normalizedRelease(release)+"/rootfs.ext4.zst", zst,
			sums["rootfs.ext4.zst"], fmt.Sprintf("Downloading Omarchy (%d of %d)...",
				len(downloadedGuestArtifacts)+1, len(downloadedGuestArtifacts)+1), ui); err != nil {
			return fmt.Errorf("preparing rootfs.ext4.zst: %w", err)
		}
		if err := requireDiskSpace(cfg.guestDir, rootfsAllocated+diskSpaceReserve); err != nil {
			return fmt.Errorf("preflighting Omarchy unpack: %w", err)
		}
		ui.setStatus("Unpacking the Omarchy system...")
		if err := decompress(zst, rootfs, sums["rootfs.ext4"], ui); err != nil {
			return fmt.Errorf("unpacking rootfs: %w", err)
		}
	}
	if err := writeInstallReceipt(cfg.guestDir, release, sumsSHA256, installedGuestArtifacts, sums); err != nil {
		return fmt.Errorf("recording verified install state: %w", err)
	}
	if removeZst {
		os.Remove(zst) // 1.4 GB nobody needs twice
	}
	ui.setStatus("Ready - starting Omarchy...")
	ui.setProgress(1, 1)
	return sleepDuringSetup(700 * time.Millisecond)
}

func releaseVersion(release string) string {
	parts := strings.Split(normalizedRelease(release), "/")
	if len(parts) == 0 {
		return currentVersion
	}
	version := parts[len(parts)-1]
	if _, ok := parsePreviewVersion(version); ok {
		return version
	}
	return currentVersion
}

func ensureVerifiedPortableCopy(src, dest, wantSum, status string, ui *progressUI) error {
	if _, err := os.Lstat(dest); err == nil {
		ui.setStatus("Checking cached %s...", filepath.Base(dest))
	} else if !os.IsNotExist(err) {
		return err
	}
	ok, err := verifyFileSHA256(dest, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if ok {
		return nil
	}
	if err := removeCachedFile(dest); err != nil {
		return err
	}
	ui.setStatus("%s", status)
	return copyPortableArtifact(src, dest, wantSum, ui.setProgress)
}

func ensureVerifiedDownload(client *http.Client, url, dest, wantSum, status string, ui *progressUI) error {
	if _, err := os.Lstat(dest); err == nil {
		ui.setStatus("Checking cached %s...", filepath.Base(dest))
	} else if !os.IsNotExist(err) {
		return err
	}
	ok, err := verifyFileSHA256(dest, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if ok {
		return nil
	}
	if err := removeCachedFile(dest); err != nil {
		return err
	}
	ui.setStatus("%s", status)
	return download(client, url, dest, wantSum, ui)
}

func removeCachedFile(path string) error {
	err := os.Remove(path)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func download(client *http.Client, url, dest, wantSum string, ui *progressUI) error {
	phase := ""
	return downloadVerified(client, url, dest, wantSum, func(next string, done, total int64) {
		if next != phase {
			if next == downloadPhaseVerify {
				ui.setStatus("Checking downloaded %s...", filepath.Base(dest))
			} else if phase == downloadPhaseVerify {
				ui.setStatus("Resuming %s...", filepath.Base(dest))
			}
		}
		phase = next
		ui.setProgress(done, total)
	})
}

func decompress(src, dest, wantSum string, ui *progressUI) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, err := in.Stat()
	if err != nil {
		return err
	}
	counted := &countingReader{r: in}
	dec, err := zstd.NewReader(counted)
	if err != nil {
		return err
	}
	defer dec.Close()
	tmp := dest + ".part"
	if err := removeCachedFile(tmp); err != nil {
		return err
	}
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	writeOK := false
	defer func() {
		if !writeOK {
			out.Close()
			os.Remove(tmp)
		}
	}()
	// The rootfs is mostly zeros. NTFS stores the skipped blocks sparsely;
	// exFAT allocates them when the file is truncated but uses the same copy
	// loop so progress continues to update during the full-size fallback.
	_ = setSparse(out)
	if err := sparseCopyStream(out, dec, st.Size(), counted, ui); err != nil {
		return err
	}
	if err := out.Sync(); err != nil {
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	ui.setStatus("Checking the unpacked Omarchy system...")
	ok, err := verifyFileSHA256(tmp, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("checksum mismatch - the unpacked image is corrupt, try again")
	}
	if err := os.Rename(tmp, dest); err != nil {
		return err
	}
	writeOK = true
	return nil
}

type countingReader struct {
	r io.Reader
	n int64
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += int64(n)
	return n, err
}

func sparseCopyStream(dst *os.File, src io.Reader, srcTotal int64, counted *countingReader, ui *progressUI) error {
	buf := make([]byte, 1<<20)
	zero := make([]byte, 1<<20)
	var off int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		n, err := io.ReadFull(src, buf)
		if n > 0 {
			if !bytes.Equal(buf[:n], zero[:n]) {
				if _, werr := dst.WriteAt(buf[:n], off); werr != nil {
					return werr
				}
			}
			off += int64(n)
			ui.setProgress(counted.n, srcTotal)
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return dst.Truncate(off)
		}
		if err != nil {
			return err
		}
	}
}
