package main

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

func fastDownloadOptions() downloadOptions {
	return downloadOptions{
		maxAttempts: 3,
		idleTimeout: 2 * time.Second,
		retryDelay:  func(int) time.Duration { return 0 },
	}
}

func runTestDownload(t *testing.T, client *http.Client, url string, payload []byte, prepare func(dest string)) string {
	t.Helper()
	dest := filepath.Join(t.TempDir(), "payload.bin")
	if prepare != nil {
		prepare(dest)
	}
	if err := downloadVerifiedWithOptions(client, url, dest, testSHA256(payload), nil, fastDownloadOptions()); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != string(payload) {
		t.Fatalf("downloaded %q, want %q", data, payload)
	}
	if _, err := os.Stat(dest + ".part"); !os.IsNotExist(err) {
		t.Fatalf("partial file remains after commit: %v", err)
	}
	return dest
}

func TestDownloadVerifiedCleanResponse(t *testing.T) {
	payload := []byte("complete payload")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("Accept-Encoding"); got != "identity" {
			t.Errorf("Accept-Encoding = %q, want identity", got)
		}
		w.Write(payload)
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, nil)
}

func TestDownloadVerifiedResumesPartialResponse(t *testing.T) {
	payload := []byte("prefix plus the remaining payload")
	prefixLength := 7
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		wantRange := fmt.Sprintf("bytes=%d-", prefixLength)
		if got := r.Header.Get("Range"); got != wantRange {
			t.Errorf("Range = %q, want %q", got, wantRange)
			return
		}
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", prefixLength, len(payload)-1, len(payload)))
		w.WriteHeader(http.StatusPartialContent)
		w.Write(payload[prefixLength:])
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, func(dest string) {
		if err := os.WriteFile(dest+".part", payload[:prefixLength], 0o644); err != nil {
			t.Fatal(err)
		}
	})
}

func TestDownloadVerifiedRestartsWhenServerIgnoresRange(t *testing.T) {
	payload := []byte("server returns the whole payload")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Range") == "" {
			t.Error("resume request did not include Range")
			return
		}
		w.Write(payload)
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, func(dest string) {
		if err := os.WriteFile(dest+".part", []byte("old prefix"), 0o644); err != nil {
			t.Fatal(err)
		}
	})
}

func TestDownloadVerifiedRestartsAfterWrongContentRange(t *testing.T) {
	payload := []byte("payload after a bad range response")
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		request := requests.Add(1)
		if request == 1 {
			if r.Header.Get("Range") == "" {
				t.Error("first request did not attempt a resume")
				return
			}
			w.Header().Set("Content-Range", fmt.Sprintf("bytes 0-%d/%d", len(payload)-1, len(payload)))
			w.WriteHeader(http.StatusPartialContent)
			return
		}
		if got := r.Header.Get("Range"); got != "" {
			t.Errorf("retry Range = %q, want a clean restart", got)
			return
		}
		w.Write(payload)
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, func(dest string) {
		if err := os.WriteFile(dest+".part", []byte("prefix"), 0o644); err != nil {
			t.Fatal(err)
		}
	})
	if requests.Load() != 2 {
		t.Fatalf("requests = %d, want 2", requests.Load())
	}
}

func TestDownloadVerifiedRestartsAfterStalePartialChecksum(t *testing.T) {
	payload := []byte("current payload bytes")
	prefixLength := 7
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch requests.Add(1) {
		case 1:
			wantRange := fmt.Sprintf("bytes=%d-", prefixLength)
			if got := r.Header.Get("Range"); got != wantRange {
				t.Errorf("Range = %q, want %q", got, wantRange)
				return
			}
			w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", prefixLength, len(payload)-1, len(payload)))
			w.WriteHeader(http.StatusPartialContent)
			_, _ = w.Write(payload[prefixLength:])
		case 2:
			if got := r.Header.Get("Range"); got != "" {
				t.Errorf("clean restart Range = %q, want none", got)
				return
			}
			_, _ = w.Write(payload)
		default:
			t.Error("unexpected extra request")
		}
	}))
	defer server.Close()
	dest := filepath.Join(t.TempDir(), "payload.bin")
	if err := os.WriteFile(dest+".part", []byte("stale!!"), 0o644); err != nil {
		t.Fatal(err)
	}
	opts := fastDownloadOptions()
	opts.maxAttempts = 1
	if err := downloadVerifiedWithOptions(server.Client(), server.URL, dest, testSHA256(payload), nil, opts); err != nil {
		t.Fatal(err)
	}
	if requests.Load() != 2 {
		t.Fatalf("requests = %d, want 2", requests.Load())
	}
	data, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != string(payload) {
		t.Fatalf("downloaded %q, want %q", data, payload)
	}
}

func TestDownloadVerifiedRetriesInterruptedResponse(t *testing.T) {
	payload := []byte(strings.Repeat("resume me ", 64))
	cut := len(payload) / 3
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		request := requests.Add(1)
		if request == 1 {
			w.Header().Set("Content-Length", strconv.Itoa(len(payload)))
			w.WriteHeader(http.StatusOK)
			w.Write(payload[:cut])
			return
		}
		wantRange := fmt.Sprintf("bytes=%d-", cut)
		if got := r.Header.Get("Range"); got != wantRange {
			t.Errorf("retry Range = %q, want %q", got, wantRange)
			return
		}
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", cut, len(payload)-1, len(payload)))
		w.WriteHeader(http.StatusPartialContent)
		w.Write(payload[cut:])
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, nil)
	if requests.Load() != 2 {
		t.Fatalf("requests = %d, want 2", requests.Load())
	}
}

func TestDownloadVerifiedRetainsPartialAcrossRuns(t *testing.T) {
	payload := []byte(strings.Repeat("keep this partial ", 32))
	cut := len(payload) / 2
	dest := filepath.Join(t.TempDir(), "payload.bin")
	interrupted := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", strconv.Itoa(len(payload)))
		w.WriteHeader(http.StatusOK)
		w.Write(payload[:cut])
	}))
	opts := fastDownloadOptions()
	opts.maxAttempts = 1
	err := downloadVerifiedWithOptions(interrupted.Client(), interrupted.URL, dest, testSHA256(payload), nil, opts)
	interrupted.Close()
	if err == nil {
		t.Fatal("interrupted response unexpectedly succeeded")
	}
	info, err := os.Stat(dest + ".part")
	if err != nil {
		t.Fatal(err)
	}
	if info.Size() != int64(cut) {
		t.Fatalf("partial size = %d, want %d", info.Size(), cut)
	}

	resumed := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		wantRange := fmt.Sprintf("bytes=%d-", cut)
		if got := r.Header.Get("Range"); got != wantRange {
			t.Errorf("Range = %q, want %q", got, wantRange)
			return
		}
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", cut, len(payload)-1, len(payload)))
		w.WriteHeader(http.StatusPartialContent)
		w.Write(payload[cut:])
	}))
	defer resumed.Close()
	if err := downloadVerifiedWithOptions(resumed.Client(), resumed.URL, dest, testSHA256(payload), nil, fastDownloadOptions()); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != string(payload) {
		t.Fatal("resumed payload does not match")
	}
}

func TestDownloadVerifiedTimesOutIdleBody(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "1")
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		time.Sleep(100 * time.Millisecond)
		w.Write([]byte("x"))
	}))
	defer server.Close()
	dest := filepath.Join(t.TempDir(), "payload.bin")
	opts := fastDownloadOptions()
	opts.maxAttempts = 1
	opts.idleTimeout = 20 * time.Millisecond
	err := downloadVerifiedWithOptions(server.Client(), server.URL, dest, testSHA256([]byte("x")), nil, opts)
	if err == nil || !strings.Contains(err.Error(), "stalled") {
		t.Fatalf("error = %v, want idle-stall failure", err)
	}
}

func TestDownloadVerifiedCancelsActiveRequest(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	started := make(chan struct{})
	client := &http.Client{Transport: roundTripFunc(func(req *http.Request) (*http.Response, error) {
		close(started)
		<-req.Context().Done()
		return nil, req.Context().Err()
	})}
	result := make(chan error, 1)
	go func() {
		result <- downloadVerifiedWithOptions(client, "https://example.invalid/payload", filepath.Join(t.TempDir(), "payload.bin"),
			testSHA256([]byte("payload")), nil, fastDownloadOptions())
	}()
	<-started
	requestSetupCancel()
	select {
	case err := <-result:
		if !errors.Is(err, errSetupCancelled) {
			t.Fatalf("error = %v, want setup cancellation", err)
		}
	case <-time.After(time.Second):
		t.Fatal("active request did not stop after cancellation")
	}
}

type blockingDownloadBody struct {
	started   chan struct{}
	closed    chan struct{}
	once      sync.Once
	closeOnce sync.Once
}

func (b *blockingDownloadBody) Read([]byte) (int, error) {
	b.once.Do(func() { close(b.started) })
	<-b.closed
	return 0, errors.New("body closed")
}

func (b *blockingDownloadBody) Close() error {
	b.once.Do(func() { close(b.started) })
	b.closeOnce.Do(func() { close(b.closed) })
	return nil
}

func TestBlockingDownloadBodyCloseIsConcurrentSafe(t *testing.T) {
	body := &blockingDownloadBody{started: make(chan struct{}), closed: make(chan struct{})}
	var wait sync.WaitGroup
	for range 16 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := body.Close(); err != nil {
				t.Errorf("Close: %v", err)
			}
		}()
	}
	wait.Wait()
}

func TestDownloadVerifiedCancelsBlockedBodyAndCleansPartial(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	body := &blockingDownloadBody{started: make(chan struct{}), closed: make(chan struct{})}
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode:    http.StatusOK,
			ContentLength: 7,
			Body:          body,
			Header:        make(http.Header),
		}, nil
	})}
	root := t.TempDir()
	dest := filepath.Join(root, "payload.bin")
	result := make(chan error, 1)
	go func() {
		result <- downloadVerifiedWithOptions(client, "https://example.invalid/payload", dest,
			testSHA256([]byte("payload")), nil, fastDownloadOptions())
	}()
	<-body.started
	requestSetupCancel()
	select {
	case err := <-result:
		if !errors.Is(err, errSetupCancelled) {
			t.Fatalf("error = %v, want setup cancellation", err)
		}
	case <-time.After(time.Second):
		t.Fatal("blocked body read did not stop after cancellation")
	}
	if _, err := os.Stat(dest + ".part"); err != nil {
		t.Fatalf("partial should remain until explicit-cancel cleanup: %v", err)
	}
	if err := cleanupCancelledSetup(root, filepath.Join(t.TempDir(), "TryOmarchy.exe"), false); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dest + ".part"); !os.IsNotExist(err) {
		t.Fatalf("explicit-cancel cleanup left partial behind: %v", err)
	}
}

func TestDownloadVerifiedCancelsRetryDelay(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusServiceUnavailable,
			Body:       io.NopCloser(strings.NewReader("retry")),
			Header:     make(http.Header),
		}, nil
	})}
	delayStarted := make(chan struct{})
	opts := fastDownloadOptions()
	opts.retryDelay = func(int) time.Duration {
		close(delayStarted)
		return time.Hour
	}
	result := make(chan error, 1)
	go func() {
		result <- downloadVerifiedWithOptions(client, "https://example.invalid/payload", filepath.Join(t.TempDir(), "payload.bin"),
			testSHA256([]byte("payload")), nil, opts)
	}()
	<-delayStarted
	requestSetupCancel()
	select {
	case err := <-result:
		if !errors.Is(err, errSetupCancelled) {
			t.Fatalf("error = %v, want setup cancellation", err)
		}
	case <-time.After(time.Second):
		t.Fatal("retry delay did not stop after cancellation")
	}
}

func TestDownloadVerifiedCommitsCompletePartAfterRangeRejection(t *testing.T) {
	payload := []byte("already complete")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		wantRange := fmt.Sprintf("bytes=%d-", len(payload))
		if got := r.Header.Get("Range"); got != wantRange {
			t.Errorf("Range = %q, want %q", got, wantRange)
			return
		}
		w.Header().Set("Content-Range", fmt.Sprintf("bytes */%d", len(payload)))
		w.WriteHeader(http.StatusRequestedRangeNotSatisfiable)
	}))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, func(dest string) {
		if err := os.WriteFile(dest+".part", payload, 0o644); err != nil {
			t.Fatal(err)
		}
	})
}

func TestDownloadVerifiedRejectsChecksumMismatch(t *testing.T) {
	want := []byte("expected payload")
	wrong := []byte("different bytes")
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		w.Write(wrong)
	}))
	defer server.Close()
	dest := filepath.Join(t.TempDir(), "payload.bin")
	err := downloadVerifiedWithOptions(server.Client(), server.URL, dest, testSHA256(want), nil, fastDownloadOptions())
	if err == nil || !strings.Contains(err.Error(), "checksum mismatch") {
		t.Fatalf("error = %v, want checksum mismatch", err)
	}
	if requests.Load() != 1 {
		t.Fatalf("requests = %d, want 1", requests.Load())
	}
	if _, err := os.Stat(dest + ".part"); !os.IsNotExist(err) {
		t.Fatalf("bad partial was retained: %v", err)
	}
}

func TestRenameDownloadedPartRetriesTransientWindowsError(t *testing.T) {
	transientErr := &os.LinkError{Op: "rename", Old: "payload.part", New: "payload", Err: syscall.Errno(32)}
	attempts := 0
	sleeps := 0
	err := renameDownloadedPart("payload.part", "payload", func(string, string) error {
		attempts++
		if attempts < 3 {
			return transientErr
		}
		return nil
	}, retryableWindowsRenameError, func(delay time.Duration) error {
		sleeps++
		if delay != commitRenameDelay {
			t.Fatalf("retry delay = %s, want %s", delay, commitRenameDelay)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if attempts != 3 || sleeps != 2 {
		t.Fatalf("attempts = %d, sleeps = %d; want 3 attempts and 2 sleeps", attempts, sleeps)
	}
}

func TestRenameDownloadedPartFailsNonTransientErrorImmediately(t *testing.T) {
	wantErr := &os.LinkError{Op: "rename", Old: "payload.part", New: "payload", Err: syscall.Errno(2)}
	attempts := 0
	sleeps := 0
	err := renameDownloadedPart("payload.part", "payload", func(string, string) error {
		attempts++
		return wantErr
	}, retryableWindowsRenameError, func(time.Duration) error {
		sleeps++
		return nil
	})
	if !errors.Is(err, wantErr) {
		t.Fatalf("error = %v, want %v", err, wantErr)
	}
	if attempts != 1 || sleeps != 0 {
		t.Fatalf("attempts = %d, sleeps = %d; want 1 attempt and no sleep", attempts, sleeps)
	}
}

func TestRenameDownloadedPartBoundsTransientRetries(t *testing.T) {
	wantErr := &os.LinkError{Op: "rename", Old: "payload.part", New: "payload", Err: syscall.Errno(5)}
	attempts := 0
	sleeps := 0
	err := renameDownloadedPart("payload.part", "payload", func(string, string) error {
		attempts++
		return wantErr
	}, retryableWindowsRenameError, func(time.Duration) error {
		sleeps++
		return nil
	})
	if !errors.Is(err, wantErr) {
		t.Fatalf("error = %v, want %v", err, wantErr)
	}
	if attempts != commitRenameAttempts || sleeps != commitRenameAttempts-1 {
		t.Fatalf("attempts = %d, sleeps = %d; want %d attempts and %d sleeps", attempts, sleeps, commitRenameAttempts, commitRenameAttempts-1)
	}
}

func TestRenameDownloadedPartCancelsRetryDelay(t *testing.T) {
	wantErr := &os.LinkError{Op: "rename", Old: "payload.part", New: "payload", Err: syscall.Errno(33)}
	attempts := 0
	err := renameDownloadedPart("payload.part", "payload", func(string, string) error {
		attempts++
		return wantErr
	}, retryableWindowsRenameError, func(time.Duration) error {
		return errSetupCancelled
	})
	if !errors.Is(err, errSetupCancelled) {
		t.Fatalf("error = %v, want setup cancellation", err)
	}
	if attempts != 1 {
		t.Fatalf("attempts = %d, want 1", attempts)
	}
}

func TestParseContentRange(t *testing.T) {
	start, end, total, ok := parseContentRange("bytes 10-19/100")
	if !ok || start != 10 || end != 19 || total != 100 {
		t.Fatalf("parsed (%d, %d, %d, %t)", start, end, total, ok)
	}
	for _, value := range []string{"", "items 1-2/3", "bytes */3", "bytes 2-1/3", "bytes 1-3/3"} {
		if _, _, _, ok := parseContentRange(value); ok {
			t.Errorf("accepted invalid Content-Range %q", value)
		}
	}
}

func TestDownloadRecoversCompleteCacheWhenServerUnavailable(t *testing.T) {
	configureSetupCancellation(false)
	payload := []byte("complete authenticated payload")
	for _, offline := range []bool{false, true} {
		t.Run(strconv.FormatBool(offline), func(t *testing.T) {
			client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
				if offline {
					return nil, errors.New("network unavailable")
				}
				return &http.Response{StatusCode: http.StatusServiceUnavailable, Body: io.NopCloser(strings.NewReader("busy")), Header: make(http.Header)}, nil
			})}
			runTestDownload(t, client, "https://example.invalid/payload", payload, func(dest string) {
				if err := os.WriteFile(dest+".part", payload, 0600); err != nil {
					t.Fatal(err)
				}
			})
		})
	}
}

func TestDownloadLeavesIncompleteCacheWhenServerUnavailable(t *testing.T) {
	configureSetupCancellation(false)
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) { return nil, errors.New("network unavailable") })}
	dest := filepath.Join(t.TempDir(), "payload")
	if err := os.WriteFile(dest+".part", []byte("prefix"), 0600); err != nil {
		t.Fatal(err)
	}
	err := downloadVerifiedWithOptions(client, "https://example.invalid/payload", dest, testSHA256([]byte("prefix and remainder")), nil, fastDownloadOptions())
	if err == nil {
		t.Fatal("incomplete cache accepted")
	}
	if _, err := os.Stat(dest); !os.IsNotExist(err) {
		t.Fatal("incomplete payload published")
	}
	data, err := os.ReadFile(dest + ".part")
	if err != nil || string(data) != "prefix" {
		t.Fatalf("partial lost: %q %v", data, err)
	}
}

type unreadDownloadBody struct{ t *testing.T }

func (b unreadDownloadBody) Read([]byte) (int, error) {
	b.t.Error("complete cache was downloaded again")
	return 0, io.EOF
}
func (unreadDownloadBody) Close() error { return nil }

func TestDownloadChecksCompleteCacheWhenRangeIgnored(t *testing.T) {
	payload := []byte("already here")
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: http.StatusOK, ContentLength: int64(len(payload)), Body: unreadDownloadBody{t}, Header: make(http.Header)}, nil
	})}
	runTestDownload(t, client, "https://example.invalid/payload", payload, func(dest string) {
		if err := os.WriteFile(dest+".part", payload, 0600); err != nil {
			t.Fatal(err)
		}
	})
}

func TestDownloadReplacesWrongSameSizeCache(t *testing.T) {
	payload := []byte("right")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Header().Set("Content-Length", "5"); w.Write(payload) }))
	defer server.Close()
	runTestDownload(t, server.Client(), server.URL, payload, func(dest string) {
		if err := os.WriteFile(dest+".part", []byte("wrong"), 0600); err != nil {
			t.Fatal(err)
		}
	})
}

type shortDownloadWriter struct{}

func (shortDownloadWriter) Write(p []byte) (int, error) { return len(p) - 1, nil }

func TestDownloadReportsShortDiskWrite(t *testing.T) {
	configureSetupCancellation(false)
	written, retry, err := copyDownloadBody(io.NopCloser(strings.NewReader("payload")), shortDownloadWriter{}, 0, 7, nil, 0)
	if !errors.Is(err, io.ErrShortWrite) || retry || written != 6 {
		t.Fatalf("got %d %t %v", written, retry, err)
	}
}

func TestDownloadDoesNotWritePastAnnouncedSize(t *testing.T) {
	configureSetupCancellation(false)
	var dst strings.Builder
	written, retry, err := copyDownloadBody(io.NopCloser(strings.NewReader("too many bytes")), &dst, 4, 7, nil, 0)
	if err == nil || retry || written != 0 || dst.Len() != 0 {
		t.Fatalf("got %d %t %v with %q written", written, retry, err, dst.String())
	}
}

func TestDownloadCacheRecoveryHonorsCancellation(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	payload := []byte("complete payload")
	dest := filepath.Join(t.TempDir(), "payload")
	if err := os.WriteFile(dest+".part", payload, 0600); err != nil {
		t.Fatal(err)
	}
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) { return nil, errors.New("offline") })}
	err := downloadVerifiedWithOptions(client, "https://example.invalid/payload", dest, testSHA256(payload), func(phase string, done, total int64) {
		if phase == downloadPhaseVerify {
			requestSetupCancel()
		}
	}, fastDownloadOptions())
	if !errors.Is(err, errSetupCancelled) {
		t.Fatalf("got %v", err)
	}
	if _, err := os.Stat(dest); !os.IsNotExist(err) {
		t.Fatal("cancelled recovery published a file")
	}
	data, err := os.ReadFile(dest + ".part")
	if err != nil || string(data) != string(payload) {
		t.Fatalf("cached transfer lost: %q %v", data, err)
	}
}
