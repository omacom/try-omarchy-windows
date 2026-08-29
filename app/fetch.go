//go:build windows

package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
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
// SHA256SUMS, and decompresses the rootfs. Reruns resume nothing fancy: a file
// that finished (name present, hash verified at download time) is kept, a
// partial temp file is redone.

var artifacts = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4.zst"}

func ensureGuest(cfg *config, release string) error {
	missing := false
	if _, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4")); err != nil {
		for _, f := range artifacts {
			if _, err := os.Stat(filepath.Join(cfg.guestDir, f)); err != nil {
				missing = true
			}
		}
	}
	needDecompress := false
	if _, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4")); err != nil {
		needDecompress = true
	}
	if !missing && !needDecompress {
		return nil
	}
	if err := os.MkdirAll(cfg.guestDir, 0o755); err != nil {
		return err
	}

	ui := getUI()
	client := &http.Client{Timeout: 0}

	sums, err := fetchSums(client, release)
	if err != nil {
		return fmt.Errorf("downloading SHA256SUMS: %w", err)
	}

	for i, f := range artifacts {
		dest := filepath.Join(cfg.guestDir, f)
		if f == "rootfs.ext4.zst" {
			// Already decompressed on a previous run - the .zst is deleted then.
			if _, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4")); err == nil {
				continue
			}
		}
		if _, err := os.Stat(dest); err == nil {
			continue
		}
		ui.setStatus("Downloading Omarchy (%d of %d)...", i+1, len(artifacts))
		if err := download(client, release+"/"+f, dest, sums[f], ui); err != nil {
			return fmt.Errorf("downloading %s: %w", f, err)
		}
	}

	zst := filepath.Join(cfg.guestDir, "rootfs.ext4.zst")
	rootfs := filepath.Join(cfg.guestDir, "rootfs.ext4")
	if _, err := os.Stat(rootfs); err != nil {
		ui.setStatus("Unpacking the Omarchy system...")
		if err := decompress(zst, rootfs, ui); err != nil {
			os.Remove(rootfs)
			return fmt.Errorf("unpacking rootfs: %w", err)
		}
		os.Remove(zst) // 1.4 GB nobody needs twice
	}
	ui.setStatus("Ready - starting Omarchy...")
	ui.setProgress(1, 1)
	time.Sleep(700 * time.Millisecond)
	return nil
}

func fetchSums(client *http.Client, release string) (map[string]string, error) {
	resp, err := client.Get(release + "/SHA256SUMS")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	sums := map[string]string{}
	sc := bufio.NewScanner(resp.Body)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) == 2 {
			sums[fields[1]] = fields[0]
		}
	}
	return sums, sc.Err()
}

func download(client *http.Client, url, dest, wantSum string, ui *progressUI) error {
	resp, err := client.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
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
	if err := f.Close(); err != nil {
		return err
	}
	if wantSum != "" && hex.EncodeToString(h.Sum(nil)) != wantSum {
		os.Remove(tmp)
		return fmt.Errorf("checksum mismatch - the download is corrupt, try again")
	}
	return os.Rename(tmp, dest)
}

func decompress(src, dest string, ui *progressUI) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, _ := in.Stat()
	counted := &countingReader{r: in}
	dec, err := zstd.NewReader(counted)
	if err != nil {
		return err
	}
	defer dec.Close()
	out, err := os.Create(dest)
	if err != nil {
		return err
	}
	// The rootfs is mostly zeros; write it sparse so 6 GB lands as ~4 GB.
	if err := setSparse(out); err == nil {
		if err := sparseCopyStream(out, dec, st.Size(), counted, ui); err != nil {
			out.Close()
			return err
		}
	} else if _, err := io.Copy(out, dec); err != nil {
		out.Close()
		return err
	}
	return out.Close()
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

