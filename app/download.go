package main

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	downloadPhaseTransfer = "download"
	downloadPhaseVerify   = "verify"
	commitRenameAttempts  = 15
	commitRenameDelay     = 500 * time.Millisecond
)

type downloadProgress func(phase string, done, total int64)

type downloadOptions struct {
	maxAttempts int
	idleTimeout time.Duration
	retryDelay  func(attempt int) time.Duration
}

func defaultDownloadOptions() downloadOptions {
	return downloadOptions{
		maxAttempts: 4,
		idleTimeout: 90 * time.Second,
		retryDelay: func(attempt int) time.Duration {
			return time.Duration(1<<(attempt-1)) * 500 * time.Millisecond
		},
	}
}

// newDownloadClient bounds connection and response-header stalls without
// imposing a total deadline on multi-gigabyte payload transfers.
func newDownloadClient() *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.ResponseHeaderTimeout = 30 * time.Second
	transport.TLSHandshakeTimeout = 15 * time.Second
	return &http.Client{Transport: transport}
}

// downloadVerified resumes a sibling .part file when the server honors byte
// ranges, then authenticates the complete file before publishing it.
func downloadVerified(client *http.Client, url, dest, wantSum string, progress downloadProgress) error {
	return downloadVerifiedWithOptions(client, url, dest, wantSum, progress, defaultDownloadOptions())
}

func downloadVerifiedWithOptions(client *http.Client, url, dest, wantSum string, progress downloadProgress, opts downloadOptions) error {
	wantSum = normalizedSHA256(wantSum)
	if !validSHA256(wantSum) {
		return fmt.Errorf("release manifest has no valid SHA256 for %s", filepath.Base(dest))
	}
	if client == nil {
		return fmt.Errorf("download client is nil")
	}
	if opts.maxAttempts < 1 {
		opts.maxAttempts = 1
	}
	var lastErr error
	cleanRestartUsed := false
	for attempt := 1; attempt <= opts.maxAttempts; {
		retry, cleanRestart, err := downloadAttempt(client, url, dest, wantSum, progress, opts.idleTimeout)
		if err == nil {
			return nil
		}
		if cleanRestart {
			if cleanRestartUsed {
				return err
			}
			cleanRestartUsed = true
			lastErr = err
			continue
		}
		if !retry {
			return err
		}
		lastErr = err
		if attempt < opts.maxAttempts && opts.retryDelay != nil {
			if err := sleepDuringSetup(opts.retryDelay(attempt)); err != nil {
				return err
			}
		}
		attempt++
	}
	// A previous run may have finished transferring before it was interrupted
	// during verification or publication. Recover that file even if the server
	// is currently unavailable. Hash only once, after retries, so incomplete
	// multi-gigabyte downloads do not get rehashed on every network failure.
	if size, err := partialFileSize(dest + ".part"); err == nil && size > 0 {
		ok, err := verifyAndCommitPart(dest+".part", dest, wantSum, progress)
		if err != nil {
			return err
		}
		if ok {
			return nil
		}
	}
	return fmt.Errorf("download failed after %d attempts: %w", opts.maxAttempts, lastErr)
}

func downloadAttempt(client *http.Client, url, dest, wantSum string, progress downloadProgress, idleTimeout time.Duration) (retry, cleanRestart bool, resultErr error) {
	if err := checkSetupCancelled(); err != nil {
		return false, false, err
	}
	tmp := dest + ".part"
	offset, err := partialFileSize(tmp)
	if err != nil {
		return false, false, err
	}
	req, err := http.NewRequestWithContext(setupContext(), http.MethodGet, url, nil)
	if err != nil {
		return false, false, err
	}
	req.Header.Set("Accept-Encoding", "identity")
	if offset > 0 {
		req.Header.Set("Range", fmt.Sprintf("bytes=%d-", offset))
	}
	resp, err := client.Do(req)
	if err != nil {
		if cancelErr := checkSetupCancelled(); cancelErr != nil {
			return false, false, cancelErr
		}
		return true, false, err
	}
	defer resp.Body.Close()

	appendPart := false
	segmentLength := resp.ContentLength
	total := resp.ContentLength
	switch resp.StatusCode {
	case http.StatusOK:
		// A complete cached transfer needs verification, not another download,
		// even when the server ignores Range. A wrong hash still restarts below.
		if offset > 0 && offset == resp.ContentLength {
			ok, err := verifyAndCommitPart(tmp, dest, wantSum, progress)
			if err != nil {
				return false, false, err
			}
			if ok {
				return false, false, nil
			}
		}
		// The server ignored Range. Restart safely using this full response.
		offset = 0
	case http.StatusPartialContent:
		start, end, completeLength, ok := parseContentRange(resp.Header.Get("Content-Range"))
		if !ok || start != offset || end < start || completeLength <= end {
			if err := resetPartialFile(tmp); err != nil {
				return false, false, err
			}
			return true, false, fmt.Errorf("server returned an invalid Content-Range for offset %d", offset)
		}
		segmentLength = end - start + 1
		if resp.ContentLength >= 0 && resp.ContentLength != segmentLength {
			if err := resetPartialFile(tmp); err != nil {
				return false, false, err
			}
			return true, false, fmt.Errorf("server returned a mismatched ranged content length")
		}
		total = completeLength
		appendPart = offset > 0
	case http.StatusRequestedRangeNotSatisfiable:
		if offset == 0 {
			return false, false, fmt.Errorf("HTTP %d", resp.StatusCode)
		}
		ok, err := verifyAndCommitPart(tmp, dest, wantSum, progress)
		if err != nil {
			return false, false, err
		}
		if ok {
			return false, false, nil
		}
		if err := resetPartialFile(tmp); err != nil {
			return false, false, err
		}
		return false, true, fmt.Errorf("server rejected the cached partial download")
	default:
		retry = resp.StatusCode == http.StatusRequestTimeout ||
			resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode >= 500
		return retry, false, fmt.Errorf("HTTP %d", resp.StatusCode)
	}

	flags := os.O_CREATE | os.O_WRONLY
	if appendPart {
		flags |= os.O_APPEND
	} else {
		flags |= os.O_TRUNC
	}
	f, err := os.OpenFile(tmp, flags, 0o644)
	if err != nil {
		return false, false, err
	}
	if appendPart {
		info, err := f.Stat()
		if err != nil {
			f.Close()
			return false, false, err
		}
		if info.Size() != offset {
			f.Close()
			return true, false, fmt.Errorf("partial download changed while resuming")
		}
	}
	if progress != nil {
		progress(downloadPhaseTransfer, offset, total)
	}
	written, retry, copyErr := copyDownloadBody(resp.Body, f, offset, total, progress, idleTimeout)
	syncErr := f.Sync()
	closeErr := f.Close()
	if syncErr != nil {
		return false, false, syncErr
	}
	if closeErr != nil {
		return false, false, closeErr
	}
	if copyErr != nil {
		return retry, false, copyErr
	}
	if segmentLength >= 0 && written != segmentLength {
		return true, false, fmt.Errorf("download ended after %d of %d response bytes", written, segmentLength)
	}
	info, err := os.Stat(tmp)
	if err != nil {
		return false, false, err
	}
	if total >= 0 && info.Size() != total {
		if info.Size() > total {
			if err := resetPartialFile(tmp); err != nil {
				return false, false, err
			}
		}
		return true, false, fmt.Errorf("partial download is %d bytes; expected %d", info.Size(), total)
	}
	ok, err := verifyAndCommitPart(tmp, dest, wantSum, progress)
	if err != nil {
		return false, false, err
	}
	if !ok {
		if appendPart {
			if err := resetPartialFile(tmp); err != nil {
				return false, false, err
			}
			return false, true, fmt.Errorf("resumed download did not match the current payload")
		}
		os.Remove(tmp)
		return false, false, fmt.Errorf("checksum mismatch - the download is corrupt, try again")
	}
	return false, false, nil
}

func partialFileSize(path string) (int64, error) {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return 0, nil
	}
	if err != nil {
		return 0, err
	}
	if !info.Mode().IsRegular() {
		return 0, fmt.Errorf("partial download is not a regular file: %s", path)
	}
	return info.Size(), nil
}

func resetPartialFile(path string) error {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	return f.Close()
}

func verifyAndCommitPart(tmp, dest, wantSum string, progress downloadProgress) (bool, error) {
	ok, err := verifyFileSHA256(tmp, wantSum, func(done, total int64) {
		if progress != nil {
			progress(downloadPhaseVerify, done, total)
		}
	})
	if err != nil || !ok {
		return ok, err
	}
	if err := renameDownloadedPart(tmp, dest, os.Rename, retryableRenameError, sleepDuringSetup); err != nil {
		return false, err
	}
	return true, nil
}

func renameDownloadedPart(tmp, dest string, rename func(string, string) error, retryable func(error) bool, sleep func(time.Duration) error) error {
	var renameErr error
	for attempt := 1; attempt <= commitRenameAttempts; attempt++ {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		if renameErr = rename(tmp, dest); renameErr == nil {
			return nil
		}
		if !retryable(renameErr) || attempt == commitRenameAttempts {
			return renameErr
		}
		if err := sleep(commitRenameDelay); err != nil {
			return err
		}
	}
	return renameErr
}

func retryableRenameError(err error) bool {
	return runtime.GOOS == "windows" && retryableWindowsRenameError(err)
}

func retryableWindowsRenameError(err error) bool {
	var errno syscall.Errno
	if !errors.As(err, &errno) {
		return false
	}
	// ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION.
	return errno == 5 || errno == 32 || errno == 33
}

func copyDownloadBody(body io.ReadCloser, dst io.Writer, offset, total int64, progress downloadProgress, idleTimeout time.Duration) (int64, bool, error) {
	var timedOut atomic.Bool
	var activity chan struct{}
	var done chan struct{}
	ctx := setupContext()
	if idleTimeout > 0 {
		activity = make(chan struct{}, 1)
		done = make(chan struct{})
		go func() {
			timer := time.NewTimer(idleTimeout)
			defer timer.Stop()
			for {
				select {
				case <-activity:
					if !timer.Stop() {
						select {
						case <-timer.C:
						default:
						}
					}
					timer.Reset(idleTimeout)
				case <-timer.C:
					timedOut.Store(true)
					body.Close()
					return
				case <-ctx.Done():
					body.Close()
					return
				case <-done:
					return
				}
			}
		}()
		defer close(done)
	}
	buf := make([]byte, 1<<20)
	var written int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return written, false, err
		}
		n, readErr := body.Read(buf)
		if n > 0 {
			if total >= 0 && int64(n) > total-offset-written {
				return written, false, fmt.Errorf("download exceeded its expected size")
			}
			count, err := dst.Write(buf[:n])
			written += int64(count)
			if err != nil {
				return written, false, err
			}
			if count != n {
				return written, false, io.ErrShortWrite
			}
			if activity != nil {
				select {
				case activity <- struct{}{}:
				default:
				}
			}
			if progress != nil {
				progress(downloadPhaseTransfer, offset+written, total)
			}
		}
		if readErr == io.EOF {
			return written, false, nil
		}
		if readErr != nil {
			if err := checkSetupCancelled(); err != nil {
				return written, false, err
			}
			if timedOut.Load() {
				return written, true, fmt.Errorf("download stalled for %s", idleTimeout)
			}
			return written, true, readErr
		}
	}
}

func parseContentRange(value string) (start, end, total int64, ok bool) {
	if !strings.HasPrefix(value, "bytes ") {
		return 0, 0, 0, false
	}
	rangeAndTotal := strings.Split(strings.TrimPrefix(value, "bytes "), "/")
	if len(rangeAndTotal) != 2 || rangeAndTotal[1] == "*" {
		return 0, 0, 0, false
	}
	bounds := strings.Split(rangeAndTotal[0], "-")
	if len(bounds) != 2 {
		return 0, 0, 0, false
	}
	start, err := strconv.ParseInt(bounds[0], 10, 64)
	if err != nil || start < 0 {
		return 0, 0, 0, false
	}
	end, err = strconv.ParseInt(bounds[1], 10, 64)
	if err != nil || end < start {
		return 0, 0, 0, false
	}
	total, err = strconv.ParseInt(rangeAndTotal[1], 10, 64)
	if err != nil || total <= end {
		return 0, 0, 0, false
	}
	return start, end, total, true
}
