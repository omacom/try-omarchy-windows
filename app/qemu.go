package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// buildQemuArgs is the argument recipe from scripts/launch-omarchy.ps1,
// unchanged: GPU mode is WINQ-EMU's stack (patched WHPX survives -cpu host;
// virtio-vga-gl IS the VGA device, so no -vga none), CPU mode is stock QEMU
// with the fastest flags upstream WHPX survives (any XSAVE/AVX feature panics
// the guest kernel) and the mandatory -vga none for virtio-gpu-pci.
func buildQemuArgs(cfg *config, cmdline string) []string {
	vm := cfg.vmDir
	args := []string{}
	// Guest RAM is sized to the machine (pickGuestMem + the memory ladder);
	// hostmem for GPU blob resources scales with it.
	mem := fmt.Sprintf("%dM", cfg.memMiB)
	hostmem := "4G"
	if cfg.memMiB < 3072 {
		hostmem = "1G"
	} else if cfg.memMiB < 4096 {
		hostmem = "2G"
	}
	if cfg.useGpu {
		args = append(args,
			"-machine", "q35,accel=whpx", "-cpu", "host", "-smp", "6", "-m", mem,
			"-device", "virtio-vga-gl,blob=on,hostmem="+hostmem+",venus=on",
			// The guest cursor is visible in the QEMU profile. Forcing SDL's host
			// cursor as well produces two pointers that separate during motion.
			// Keep only the guest cursor unless the diagnostic fallback is set.
			// window-close=off: the X must not hard-kill a running OS; the
			// close guard intercepts the click and confirms + shuts down
			// gracefully instead (closeguard.go).
			"-display", sdlDisplay(true, cfg.hostCursor),
			"-serial", "file:"+filepath.Join(vm, "serial-gpu.log"),
		)
	} else {
		args = append(args,
			"-machine", "q35,accel=whpx", "-cpu", "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes",
			"-smp", "6", "-m", mem,
			"-vga", "none", "-device", "virtio-gpu-pci,id=gpu0",
			"-display", sdlDisplay(false, cfg.hostCursor),
			"-serial", "file:"+filepath.Join(vm, "serial.log"),
		)
	}
	args = append(args,
		"-drive", "file="+cfg.disk+",format=raw,if=virtio",
		"-kernel", filepath.Join(cfg.guestDir, "vmlinuz-linux"),
		"-initrd", filepath.Join(cfg.guestDir, "initramfs-linux.img"),
		"-append", cmdline,
		"-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
		"-device", "virtio-net-pci,netdev=n0", "-netdev", "user,id=n0",
		"-device", "virtio-rng-pci",
		// The backend must be explicit: with no audiodev the guest's PipeWire
		// stalls on virtio-snd control messages and the whole session hangs.
		// cfg.audio is dsound normally; "none" on machines where DirectSound
		// has no device (QEMU exits at startup otherwise).
		"-audiodev", cfg.audio+",id=snd",
		"-device", "virtio-sound-pci,audiodev=snd",
		"-qmp", fmt.Sprintf("tcp:127.0.0.1:%d,server=on,wait=off", qmpToolsPort),
		"-qmp", fmt.Sprintf("tcp:127.0.0.1:%d,server=on,wait=off", qmpFwdPort),
		"-qmp", fmt.Sprintf("tcp:127.0.0.1:%d,server=on,wait=off", qmpSupPort),
		"-D", filepath.Join(vm, "qemu.log"),
		// In-guest reboot/poweroff wedges upstream WHPX (vCPUs never return
		// from system reset). Exit instead; the supervisor relaunches on reset.
		"-no-reboot",
		"-name", appTitle,
	)
	if cfg.share != "" {
		if cfg.useGpu {
			args = append(args, "-virtfs", "local,path="+cfg.share+",mount_tag=hostshare,security_model=none")
		}
		// Stock QEMU for Windows ships no virtio-9p; silently launching
		// without the share would be confusing, so main() rejects that combo.
	}
	if cfg.fullscreen {
		args = append(args, "-full-screen")
	}
	return args
}

func sdlDisplay(gpu, hostCursor bool) string {
	gl := "off"
	if gpu {
		gl = "on"
	}
	cursor := "off"
	if hostCursor {
		cursor = "on"
	}
	return "sdl,gl=" + gl + ",show-cursor=" + cursor + ",window-close=off"
}

// prepareDisk gives the guest its writable disk: a sparse copy of the factory
// rootfs, extended to the spec's expanded size (the NTFS sparse-file trick from
// jorge's fork - the file reads as 24 GiB without occupying it). The copy takes
// a minute or two, so it gets the same progress window the download uses -
// launch must never look hung.
func prepareDisk(cfg *config, expandedMiB int64) error {
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	if expandedMiB <= 0 || expandedMiB > (1<<63-1)/(1024*1024) {
		return fmt.Errorf("invalid expanded disk size: %d MiB", expandedMiB)
	}
	expandedBytes := expandedMiB * 1024 * 1024
	if cfg.fresh {
		if err := os.Remove(cfg.disk); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("discarding the existing disk: %w", err)
		}
	}
	if info, err := os.Stat(cfg.disk); err == nil {
		if !info.Mode().IsRegular() {
			return fmt.Errorf("disk path is not a regular file: %s", cfg.disk)
		}
		if info.Size() >= expandedBytes {
			return nil
		}
		// Older launchers wrote directly to disk.raw, so an interrupted first
		// copy can exist under the final name. Preserve it for inspection while
		// rebuilding a complete disk atomically.
		quarantine := fmt.Sprintf("%s.incomplete-%d", cfg.disk, time.Now().UnixNano())
		if err := os.Rename(cfg.disk, quarantine); err != nil {
			return fmt.Errorf("quarantining incomplete disk: %w", err)
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	tmp := cfg.disk + ".part"
	if err := os.Remove(tmp); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("removing stale disk staging file: %w", err)
	}
	src, err := os.Open(filepath.Join(cfg.guestDir, "rootfs.ext4"))
	if err != nil {
		return err
	}
	defer src.Close()
	dst, err := os.Create(tmp)
	if err != nil {
		return err
	}
	complete := false
	defer func() {
		if !complete {
			dst.Close()
			os.Remove(tmp)
		}
	}()
	if err := setSparse(dst); err != nil {
		return fmt.Errorf("marking disk sparse: %w", err)
	}
	ui := getUI()
	ui.setStatus("Preparing your Omarchy disk...")
	st, err := src.Stat()
	if err != nil {
		return err
	}
	if err := sparseCopy(dst, src, st.Size(), ui); err != nil {
		return err
	}
	if err := dst.Truncate(expandedBytes); err != nil {
		return err
	}
	if err := dst.Sync(); err != nil {
		return err
	}
	if err := dst.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, cfg.disk); err != nil {
		return err
	}
	complete = true
	return nil
}
