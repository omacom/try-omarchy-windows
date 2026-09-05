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
	appTitle = "Try Omarchy"
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
	supportsSharing          bool
	audio                    string
	memMiB                   int
	forwards                 []portForward
	sshKey                   string
	// Guest RAM chosen by the user (settings.json or -memory); 0 = automatic.
	memOverrideMiB int
	diskGiB        int
	irqchipOff     bool
	cpuOverride    int
	cpus           int
	hostTotalMiB   int
	// Rendering decision inputs, see render_probe.go.
	renderMode    string
	runtimeID     string
	displayDriver string
}

type progressUI struct{}

func getUI() *progressUI                             { return &progressUI{} }
func (*progressUI) setStatus(string, ...any)         {}
func (*progressUI) setProgress(current, total int64) {}
func logf(string, ...any)                            {}
func setSparse(*os.File) error                       { return nil }
func displayDriverIdentity() string                  { return "" }
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

func TestPrepareDiskRebuildsLegacyPartialCopy(t *testing.T) {
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
	data, err := os.ReadFile(disk)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) != 1<<20 || string(data[:len("factory rootfs")]) != "factory rootfs" {
		t.Fatalf("rebuilt disk = size %d prefix %q", len(data), data[:len("factory rootfs")])
	}
	matches, err := filepath.Glob(disk + ".incomplete-*")
	if err != nil {
		t.Fatal(err)
	}
	if len(matches) != 1 {
		t.Fatalf("quarantined disks = %d, want 1", len(matches))
	}
	old, err := os.ReadFile(matches[0])
	if err != nil {
		t.Fatal(err)
	}
	if string(old) != "partial" {
		t.Fatalf("quarantined contents = %q", old)
	}
}

func TestPrepareDiskGrowsCompleteOlderDiskWithoutReplacingIt(t *testing.T) {
	dir := t.TempDir()
	guestDir := filepath.Join(dir, "guest")
	vmDir := filepath.Join(dir, "vm")
	if err := os.MkdirAll(guestDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(vmDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(guestDir, "rootfs.ext4"), []byte("factory"), 0o644); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vmDir, "disk.raw")
	original := []byte("older complete disk")
	if err := os.WriteFile(disk, original, 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{guestDir: guestDir, vmDir: vmDir, disk: disk, diskFormat: "raw"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(disk)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) != 1<<20 || string(data[:len(original)]) != string(original) {
		t.Fatalf("grown disk lost its existing prefix: size=%d prefix=%q", len(data), data[:len(original)])
	}
	if matches, err := filepath.Glob(disk + ".incomplete-*"); err != nil || len(matches) != 0 {
		t.Fatalf("complete disk was quarantined: matches=%v err=%v", matches, err)
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

func TestAudioUnavailableRecognizesQEMUDSoundErrorsOnly(t *testing.T) {
	cfg := &config{vmDir: t.TempDir()}
	log := filepath.Join(cfg.vmDir, "qemu-stderr.log")
	for _, message := range []string{
		"dsound: Could not initialize playback: No sound device\n",
		"DirectSound subsystem could not allocate resources\n",
		"Failed to initialize dsound audio driver\n",
	} {
		if err := os.WriteFile(log, []byte(message), 0o644); err != nil {
			t.Fatal(err)
		}
		if !audioUnavailable(cfg) {
			t.Fatalf("audio error was not recognized: %q", message)
		}
	}
	if err := os.WriteFile(log, []byte("WHPX: cannot set up guest memory\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if audioUnavailable(cfg) {
		t.Fatal("unrelated startup failure was classified as an audio error")
	}
}

func TestAudioUnavailableMatchesOnlyDirectSoundStartupFailures(t *testing.T) {
	cfg := &config{vmDir: t.TempDir()}
	log := filepath.Join(cfg.vmDir, "qemu-stderr.log")
	for message, want := range map[string]bool{
		"Could not initialize DirectSound: no device": true,
		"audio: Could not init dsound audio driver":   true,
		"cannot set up guest memory":                  false,
		"SDL failed to create an OpenGL context":      false,
	} {
		if err := os.WriteFile(log, []byte(message), 0o644); err != nil {
			t.Fatal(err)
		}
		if got := audioUnavailable(cfg); got != want {
			t.Fatalf("audioUnavailable(%q) = %v, want %v", message, got, want)
		}
	}
}

func TestBuildQemuArgsForwardsPortsOnLoopback(t *testing.T) {
	cfg := &config{vmDir: "/vm", guestDir: "/guest", disk: "/vm/disk.raw", diskFormat: "raw",
		memMiB: 4096, audio: "none", forwards: []portForward{{"tcp", 2222, 22}}}
	args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if !strings.Contains(args, "-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 ") {
		t.Fatalf("forward missing from QEMU args: %s", args)
	}
	cfg.forwards = nil
	args = strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if !strings.Contains(args, "-netdev user,id=n0 ") {
		t.Fatalf("plain netdev missing: %s", args)
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

func TestBuildQemuArgsEscapesCommasInsidePaths(t *testing.T) {
	cfg := &config{
		vmDir: "/vm", guestDir: "/guest", disk: "/data,one/disk.raw", diskFormat: "raw",
		share: "/host/Work,Notes", useGpu: false, supportsSharing: true, memMiB: 4096, audio: "none",
	}
	args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if !strings.Contains(args, "file=/data,,one/disk.raw,format=raw") {
		t.Fatalf("disk comma was not escaped: %s", args)
	}
	if !strings.Contains(args, "local,path=/host/Work,,Notes,mount_tag=hostshare") {
		t.Fatalf("share comma was not escaped: %s", args)
	}
}

func TestBuildQemuArgsUsesTheChosenCPUCountAndHostMem(t *testing.T) {
	cfg := &config{vmDir: "/vm", guestDir: "/guest", disk: "/vm/disk.raw", diskFormat: "raw",
		memMiB: 8192, hostTotalMiB: 32768, cpus: 6, audio: "none", useGpu: true}
	args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if !strings.Contains(args, " -smp 6 -m 8192M ") || !strings.Contains(args, "hostmem=8G") {
		t.Fatalf("cpu count or hostmem missing: %s", args)
	}
	cfg.cpus = 0
	if args = strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " "); !strings.Contains(args, " -smp 2 ") {
		t.Fatalf("unset cpu count should fall back to the minimum: %s", args)
	}
}
