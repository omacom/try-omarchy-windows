package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

const (
	portableRenameAttempts = 15
	portableRenameDelay    = 200 * time.Millisecond
)

func automaticUpdatesEnabled(cfg *config, noUpdate bool, release, sumsSHA256 string) bool {
	return !cfg.portable && !noUpdate &&
		normalizedRelease(release) == defaultReleaseURL &&
		normalizedSHA256(sumsSHA256) == defaultSumsSHA256
}

func releaseSumsForConfig(cfg *config, client *http.Client, release, expectedSHA256 string) (map[string]string, error) {
	if !cfg.portable {
		return releaseSums(client, release, expectedSHA256)
	}
	return readPortableManifest(filepath.Join(cfg.payloadDir, "SHA256SUMS"), expectedSHA256)
}

// readPortableManifest applies the same independent trust root and structural
// limits as the online path. A manifest travelling beside the payload is not
// trusted merely because both files are on the same USB.
func readPortableManifest(path, expectedSHA256 string) (map[string]string, error) {
	if err := checkSetupCancelled(); err != nil {
		return nil, err
	}
	before, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !before.Mode().IsRegular() {
		return nil, fmt.Errorf("portable SHA256SUMS is not a regular file")
	}
	if before.Size() > maxSumsBytes {
		return nil, fmt.Errorf("SHA256SUMS exceeds %d bytes", maxSumsBytes)
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(setupReader{r: f}, maxSumsBytes+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxSumsBytes {
		return nil, fmt.Errorf("SHA256SUMS exceeds %d bytes", maxSumsBytes)
	}
	after, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if int64(len(data)) != before.Size() || after.Size() != before.Size() ||
		after.ModTime().UnixNano() != before.ModTime().UnixNano() {
		return nil, fmt.Errorf("portable SHA256SUMS changed while being read")
	}
	return parseVerifiedSums(data, expectedSHA256)
}

// copyPortableArtifact verifies while copying into a sibling staging file,
// flushes it, and only then publishes the destination. Cancellation or a bad
// checksum cannot leave a file under its final name.
func copyPortableArtifact(src, dest, want string, progress func(done, total int64)) error {
	want = normalizedSHA256(want)
	if !validSHA256(want) {
		return fmt.Errorf("release manifest has no valid SHA256 for %s", filepath.Base(src))
	}
	before, err := os.Lstat(src)
	if err != nil {
		return err
	}
	if !before.Mode().IsRegular() {
		return fmt.Errorf("portable payload is not a regular file: %s", filepath.Base(src))
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	tmp := dest + ".part"
	if err := os.Remove(tmp); err != nil && !os.IsNotExist(err) {
		return err
	}
	out, err := os.OpenFile(tmp, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			out.Close()
			os.Remove(tmp)
		}
	}()
	h := sha256.New()
	buf := make([]byte, 1<<20)
	var done int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		n, readErr := in.Read(buf)
		if n > 0 {
			if _, err := out.Write(buf[:n]); err != nil {
				return err
			}
			if _, err := h.Write(buf[:n]); err != nil {
				return err
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
			return readErr
		}
	}
	after, err := in.Stat()
	if err != nil {
		return err
	}
	if done != before.Size() || after.Size() != before.Size() ||
		after.ModTime().UnixNano() != before.ModTime().UnixNano() {
		return fmt.Errorf("portable payload changed while being copied: %s", filepath.Base(src))
	}
	if hex.EncodeToString(h.Sum(nil)) != want {
		return fmt.Errorf("checksum mismatch for %s", filepath.Base(src))
	}
	if err := out.Sync(); err != nil {
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := renamePortableFileWithRetry(tmp, dest); err != nil {
		return err
	}
	ok = true
	return nil
}

// renamePortableFileWithRetry tolerates short-lived file locks from Defender
// and indexers while keeping publication bounded and setup cancellation-aware.
func renamePortableFileWithRetry(from, to string) error {
	return renamePortableFileWith(
		from,
		to,
		os.Rename,
		sleepDuringSetup,
	)
}

func renamePortableFileWith(
	from, to string,
	rename func(string, string) error,
	sleep func(time.Duration) error,
) error {
	var renameErr error
	for attempt := 0; attempt < portableRenameAttempts; attempt++ {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		if renameErr = rename(from, to); renameErr == nil {
			return nil
		}
		if attempt+1 < portableRenameAttempts {
			if err := sleep(portableRenameDelay); err != nil {
				return err
			}
		}
	}
	return renameErr
}
