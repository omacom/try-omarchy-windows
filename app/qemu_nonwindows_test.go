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
	irqchipOff               bool
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

func TestPrepareDiskRejectsInsufficientAllocatedSpace(t *testing.T) {
	dir := t.TempDir()
	guestDir := filepath.Join(dir, "guest")
	vmDir := filepath.Join(dir, "vm")
	if err := os.MkdirAll(guestDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(vmDir, 0o755); err != nil {
		t.Fatal(err)
	}
	rootfs := filepath.Join(guestDir, "rootfs.ext4")
	if err := os.WriteFile(rootfs, []byte("factory rootfs"), 0o644); err != nil {
		t.Fatal(err)
	}
	originalFree := diskFreeBytes
	originalAllocated := allocatedFileBytes
	diskFreeBytes = func(string) (int64, error) { return 2 << 30, nil }
	allocatedFileBytes = func(string) (int64, error) { return 2 << 30, nil }
	t.Cleanup(func() {
		diskFreeBytes = originalFree
		allocatedFileBytes = originalAllocated
	})
	cfg := &config{guestDir: guestDir, vmDir: vmDir, disk: filepath.Join(vmDir, "disk.raw")}
	err := prepareDisk(cfg, 4*1024)
	if err == nil || !strings.Contains(err.Error(), "preflighting writable disk storage") {
		t.Fatalf("error = %v, want disk-space failure", err)
	}
	if _, statErr := os.Stat(cfg.disk + ".part"); !os.IsNotExist(statErr) {
		t.Fatalf("disk staging file exists after preflight: %v", statErr)
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

func TestBuildQemuArgsKeepsKernelIrqchipUnlessRefused(t *testing.T) {
	for _, gpu := range []bool{true, false} {
		cfg := &config{vmDir: "/vm", guestDir: "/guest", disk: "/vm/disk.raw",
			diskFormat: "raw", memMiB: 4096, audio: "none", useGpu: gpu}
		args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
		if !strings.Contains(args, "-machine q35,accel=whpx -cpu") {
			t.Fatalf("gpu=%v: default machine missing: %s", gpu, args)
		}
		cfg.irqchipOff = true
		args = strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
		if !strings.Contains(args, "-machine q35,accel=whpx,kernel-irqchip=off -cpu") {
			t.Fatalf("gpu=%v: kernel-irqchip=off missing: %s", gpu, args)
		}
	}
}

func TestNestedVirtRefusedMatchesOnlyTheFatalForm(t *testing.T) {
	cfg := &config{vmDir: t.TempDir()}
	log := filepath.Join(cfg.vmDir, "qemu-stderr.log")
	if nestedVirtRefused(cfg) {
		t.Fatal("missing log reported a refusal")
	}
	fatal := "qemu-system-x86_64w.exe: WHPX: Failed to enable nested virtualization, hr=80370302\n" +
		"qemu-system-x86_64w.exe: failed to initialize whpx: Invalid argument\n"
	if err := os.WriteFile(log, []byte(fatal), 0o644); err != nil {
		t.Fatal(err)
	}
	if !nestedVirtRefused(cfg) {
		t.Fatal("fatal refusal not recognised")
	}
	patched := "qemu-system-x86_64w.exe: warning: WHPX: nested virtualization unavailable (hr=80370302), continuing without it\n"
	if err := os.WriteFile(log, []byte(patched), 0o644); err != nil {
		t.Fatal(err)
	}
	if nestedVirtRefused(cfg) {
		t.Fatal("patched runtime warning treated as a refusal")
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
