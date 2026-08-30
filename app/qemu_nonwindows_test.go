//go:build !windows

package main

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	appTitle     = "Try Omarchy"
	qmpToolsPort = 4445
	qmpFwdPort   = 4446
	qmpSupPort   = 4447
)

type config struct {
	dir, hostDir, payloadDir string
	winqEmu, share           string
	fresh, fullscreen, noGpu bool
	hostCursor               bool
	instant, portable        bool
	guestDir, vmDir, disk    string
	diskFormat               string
	qemu                     string
	useGpu                   bool
	audio                    string
	memMiB                   int
}

type progressUI struct{}

func getUI() *progressUI                             { return &progressUI{} }
func (*progressUI) setStatus(string, ...any)         {}
func (*progressUI) setProgress(current, total int64) {}
func logf(string, ...any)                            {}
func setSparse(*os.File) error                       { return nil }
func sparseCopy(dst, src *os.File, total int64, ui *progressUI) error {
	_, err := io.Copy(dst, src)
	return err
}

func TestPrepareDiskPublishesCompleteFile(t *testing.T) {
	dir := t.TempDir()
	guestDir := filepath.Join(dir, "guest")
	vmDir := filepath.Join(dir, "vm")
	if err := os.MkdirAll(guestDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(vmDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(guestDir, "rootfs.ext4"), []byte("factory rootfs"), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{guestDir: guestDir, vmDir: vmDir, disk: filepath.Join(vmDir, "disk.raw"), diskFormat: "raw"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	if info.Size() != 1024*1024 {
		t.Fatalf("disk size = %d, want %d", info.Size(), 1024*1024)
	}
	if _, err := os.Stat(cfg.disk + ".part"); !os.IsNotExist(err) {
		t.Fatalf("staging file remains after commit: %v", err)
	}
}

func TestSDLDisplayUsesOnlyGuestCursorByDefault(t *testing.T) {
	if got := sdlDisplay(true, false); got != "sdl,gl=on,show-cursor=off,window-close=off" {
		t.Fatalf("GPU display = %q", got)
	}
	if got := sdlDisplay(false, false); got != "sdl,gl=off,show-cursor=off,window-close=off" {
		t.Fatalf("CPU display = %q", got)
	}
	if got := sdlDisplay(true, true); got != "sdl,gl=on,show-cursor=on,window-close=off" {
		t.Fatalf("host cursor fallback = %q", got)
	}
}

func TestPrepareDiskQuarantinesLegacyPartialFile(t *testing.T) {
	dir := t.TempDir()
	guestDir := filepath.Join(dir, "guest")
	vmDir := filepath.Join(dir, "vm")
	if err := os.MkdirAll(guestDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(vmDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(guestDir, "rootfs.ext4"), []byte("factory rootfs"), 0o644); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vmDir, "disk.raw")
	if err := os.WriteFile(disk, []byte("partial"), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{guestDir: guestDir, vmDir: vmDir, disk: disk, diskFormat: "raw"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	matches, err := filepath.Glob(disk + ".incomplete-*")
	if err != nil {
		t.Fatal(err)
	}
	if len(matches) != 1 {
		t.Fatalf("quarantined disks = %d, want 1", len(matches))
	}
	data, err := os.ReadFile(matches[0])
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "partial" {
		t.Fatalf("quarantined content = %q, want partial", data)
	}
}

func TestBuildQemuArgsUsesConfiguredDiskFormat(t *testing.T) {
	cfg := &config{
		vmDir:      "/usb/data/vm",
		guestDir:   "/usb/data/guest",
		disk:       "/usb/data/vm/disk.qcow2",
		diskFormat: "qcow2",
		memMiB:     4096,
		audio:      "none",
	}
	args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if !strings.Contains(args, "file=/usb/data/vm/disk.qcow2,format=qcow2,if=virtio") {
		t.Fatalf("configured disk format missing from QEMU args: %s", args)
	}
}
