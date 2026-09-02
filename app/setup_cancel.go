package main

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	errSetupCancelled  = errors.New("setup cancelled")
	setupCancelPending atomic.Bool
	cancelRemovesAll   atomic.Bool
	setupCancelWake    = make(chan struct{}, 1)
	setupContextMu     sync.RWMutex
	setupCtx           = context.Background()
	setupContextCancel = func() {}
)

func configureSetupCancellation(removeAll bool) {
	setupContextMu.Lock()
	setupContextCancel()
	setupCtx, setupContextCancel = context.WithCancel(context.Background())
	setupContextMu.Unlock()
	setupCancelPending.Store(false)
	cancelRemovesAll.Store(removeAll)
	select {
	case <-setupCancelWake:
	default:
	}
}

func requestSetupCancel() {
	setupCancelPending.Store(true)
	setupContextMu.RLock()
	cancel := setupContextCancel
	setupContextMu.RUnlock()
	cancel()
	select {
	case setupCancelWake <- struct{}{}:
	default:
	}
}

func setupContext() context.Context {
	setupContextMu.RLock()
	ctx := setupCtx
	setupContextMu.RUnlock()
	return ctx
}

func setupCancelled() bool {
	return setupCancelPending.Load()
}

func checkSetupCancelled() error {
	if setupCancelled() {
		return errSetupCancelled
	}
	return nil
}

func sleepDuringSetup(d time.Duration) error {
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-setupContext().Done():
		return errSetupCancelled
	case <-timer.C:
		return checkSetupCancelled()
	}
}

type setupReader struct {
	r io.Reader
}

func (r setupReader) Read(p []byte) (int, error) {
	if err := checkSetupCancelled(); err != nil {
		return 0, err
	}
	return r.r.Read(p)
}

func completeInstallExists(dir, diskName string) bool {
	for _, name := range []string{
		filepath.Join("guest", "build-spec.json"),
		filepath.Join("guest", "rootfs.ext4"),
		filepath.Join("vm", diskName),
	} {
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil || !info.Mode().IsRegular() {
			return false
		}
	}
	return true
}

func cleanupCancelledSetup(dir, executable string, removeAll bool) error {
	if removeAll {
		return removeInstallExceptExecutable(dir, executable)
	}
	return filepath.WalkDir(dir, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			if os.IsNotExist(walkErr) {
				return nil
			}
			return walkErr
		}
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".part") {
			return nil
		}
		return os.Remove(path)
	})
}

func removeInstallExceptExecutable(dir, executable string) error {
	root, err := filepath.Abs(dir)
	if err != nil {
		return err
	}
	keep, err := filepath.Abs(executable)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(root, keep)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return removeAllWithRetry(root)
	}
	return removeChildrenExcept(root, keep)
}

func removeChildrenExcept(dir, keep string) error {
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		path := filepath.Join(dir, entry.Name())
		if pathsEqual(path, keep) {
			continue
		}
		rel, err := filepath.Rel(path, keep)
		containsKeep := err == nil && rel != "." && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
		if containsKeep && entry.IsDir() {
			if err := removeChildrenExcept(path, keep); err != nil {
				return err
			}
			continue
		}
		if err := removeAllWithRetry(path); err != nil {
			return err
		}
	}
	return nil
}

func pathsEqual(a, b string) bool {
	a, b = filepath.Clean(a), filepath.Clean(b)
	if runtime.GOOS == "windows" {
		return strings.EqualFold(a, b)
	}
	return a == b
}

func removeAllWithRetry(path string) error {
	var err error
	for attempt := 0; attempt < 15; attempt++ {
		if err = os.RemoveAll(path); err == nil {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return err
}
