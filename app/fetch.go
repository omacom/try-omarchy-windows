//go:build windows

package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/klauspost/compress/zstd"
)

// First-run setup: the exe IS the stub - it downloads the guest image from the
// GitHub release on first launch (with a progress window), verifies it against
// the authenticated SHA256SUMS, and decompresses the rootfs.

var (
	downloadedGuestArtifacts = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img"}
	installedGuestArtifacts  = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4"}
)

func ensureGuest(cfg *config, release, sumsSHA256 string) error {
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
	client := &http.Client{Timeout: 0}

	sums, err := releaseSums(client, release, sumsSHA256)
	if err != nil {
		return fmt.Errorf("authenticating SHA256SUMS: %w", err)
	}
	for _, name := range []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4", "rootfs.ext4.zst"} {
		if !validSHA256(sums[name]) {
			return fmt.Errorf("release manifest has no valid SHA256 for %s", name)
		}
	}

	for i, name := range downloadedGuestArtifacts {
		dest := filepath.Join(cfg.guestDir, name)
		if err := ensureVerifiedDownload(client, normalizedRelease(release)+"/"+name, dest, sums[name],
			fmt.Sprintf("Downloading Omarchy (%d of %d)...", i+1, len(downloadedGuestArtifacts)+1), ui); err != nil {
			return fmt.Errorf("preparing %s: %w", name, err)
		}
	}

	zst := filepath.Join(cfg.guestDir, "rootfs.ext4.zst")
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
		if err := ensureVerifiedDownload(client, normalizedRelease(release)+"/rootfs.ext4.zst", zst,
			sums["rootfs.ext4.zst"], fmt.Sprintf("Downloading Omarchy (%d of %d)...",
				len(downloadedGuestArtifacts)+1, len(downloadedGuestArtifacts)+1), ui); err != nil {
			return fmt.Errorf("preparing rootfs.ext4.zst: %w", err)
		}
		ui.setStatus("Unpacking the Omarchy system...")
		if err := decompress(zst, rootfs, sums["rootfs.ext4"], ui); err != nil {
			return fmt.Errorf("unpacking rootfs: %w", err)
		}
	}
	if err := writeInstallReceipt(cfg.guestDir, release, sumsSHA256, installedGuestArtifacts, sums); err != nil {
		return fmt.Errorf("recording verified install state: %w", err)
	}
	os.Remove(zst) // 1.4 GB nobody needs twice
	ui.setStatus("Ready - starting Omarchy...")
	ui.setProgress(1, 1)
	time.Sleep(700 * time.Millisecond)
	return nil
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
	if !validSHA256(wantSum) {
		return fmt.Errorf("release manifest has no valid SHA256 for %s", filepath.Base(dest))
	}
	resp, err := client.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	tmp := dest + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	h := sha256.New()
	buf := make([]byte, 1<<20)
	var done int64
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				f.Close()
				return werr
			}
			h.Write(buf[:n])
			done += int64(n)
			ui.setProgress(done, resp.ContentLength)
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			f.Close()
			return rerr
		}
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if hex.EncodeToString(h.Sum(nil)) != wantSum {
		os.Remove(tmp)
		return fmt.Errorf("checksum mismatch - the download is corrupt, try again")
	}
	return os.Rename(tmp, dest)
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
	// The rootfs is mostly zeros; write it sparse so 6 GB lands as ~4 GB.
	if err := setSparse(out); err == nil {
		if err := sparseCopyStream(out, dec, st.Size(), counted, ui); err != nil {
			return err
		}
	} else if _, err := io.Copy(out, dec); err != nil {
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
