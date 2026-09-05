package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
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
	hostmem := gpuHostMem(cfg.memMiB, cfg.hostTotalMiB)
	smp := fmt.Sprint(cfg.cpus)
	if cfg.cpus <= 0 {
		smp = fmt.Sprint(minimumAutoGuestCPUs)
	}
	machine := "q35,accel=whpx"
	if cfg.irqchipOff {
		machine += ",kernel-irqchip=off"
	}
	if cfg.useGpu {
		args = append(args,
			"-machine", machine, "-cpu", "host", "-smp", smp, "-m", mem,
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
			"-machine", machine, "-cpu", "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes",
			"-smp", smp, "-m", mem,
			"-vga", "none", "-device", "virtio-gpu-pci,id=gpu0",
			"-display", sdlDisplay(false, cfg.hostCursor),
			"-serial", "file:"+filepath.Join(vm, "serial.log"),
		)
	}
	// The guest tunes its desktop to the rendering path: on llvmpipe every
	// animation frame is CPU time, so the image turns animations off unless
	// the user's own config turns them back on.
	render := "gpu"
	if !cfg.useGpu {
		render = "cpu"
	}
	args = append(args,
		"-drive", "file="+qemuOptionValue(cfg.disk)+",format="+cfg.diskFormat+",if=virtio",
		"-kernel", filepath.Join(cfg.guestDir, "vmlinuz-linux"),
		"-initrd", filepath.Join(cfg.guestDir, "initramfs-linux.img"),
		"-append", cmdline+" tryomarchy.render="+render,
		"-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
		"-device", "virtio-net-pci,netdev=n0", "-netdev", netdevArg(cfg.forwards),
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
		if cfg.supportsSharing {
			args = append(args, "-virtfs", "local,path="+qemuOptionValue(cfg.share)+",mount_tag=hostshare,security_model=none")
		}
		// Stock QEMU for Windows ships no virtio-9p. main() selects the bundled
		// runtime when possible and otherwise tells the user before continuing.
	}
	if cfg.fullscreen {
		args = append(args, "-full-screen")
	}
	return args
}

// QEMU's structured option parser uses commas as separators and represents a
// literal comma as two commas. Paths are already separate argv values, but a
// folder such as C:\Work,Notes still needs this option-level escaping.
func qemuOptionValue(value string) string {
	return strings.ReplaceAll(value, ",", ",,")
}

// nestedVirtRefused reports whether the current attempt's QEMU died because
// the host advertised nested virtualization and then refused to enable it
// for the partition (hr=80370302 on Meteor Lake laptops and hosts running
// the full Hyper-V feature set). QEMU only asks for it with the kernel
// irqchip, so the retry with kernel-irqchip=off sidesteps the request. The
// source-built runtime also downgrades this to a warning with different
// wording, so a patched runtime never matches here.
func nestedVirtRefused(cfg *config) bool {
	data, err := os.ReadFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"))
	return err == nil && bytes.Contains(data, []byte("Failed to enable nested virtualization"))
}

// audioUnavailable distinguishes a missing DirectSound device from unrelated
// startup failures. Retrying every failure without audio used to consume a
// fallback attempt even when the real problem was memory or graphics.
func audioUnavailable(cfg *config) bool {
	data, err := os.ReadFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"))
	if err != nil {
		return false
	}
	message := bytes.ToLower(data)
	return bytes.Contains(message, []byte("dsound:")) ||
		bytes.Contains(message, []byte("directsound")) ||
		bytes.Contains(message, []byte("dsound audio driver"))
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
	expandedMiB, err := requestedDiskMiB(expandedMiB, cfg.diskGiB, cfg.portable)
	if err != nil {
		return err
	}
	expandedBytes := expandedMiB * 1024 * 1024
	if cfg.fresh && !cfg.portable {
		_, err := resetStandardDisk(cfg, expandedMiB)
		return err
	}
	if cfg.fresh && cfg.portable {
		return resetPortableDisk(cfg, expandedBytes)
	}
	if cfg.portable {
		return preparePortableDisk(cfg, expandedBytes)
	}
	if info, err := os.Stat(cfg.disk); err == nil {
		if !info.Mode().IsRegular() {
			return fmt.Errorf("disk path is not a regular file: %s", cfg.disk)
		}
		if info.Size() >= expandedBytes {
			return nil
		}
		factoryInfo, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4"))
		if err != nil {
			return fmt.Errorf("measuring the factory disk: %w", err)
		}
		if !factoryInfo.Mode().IsRegular() {
			return fmt.Errorf("factory disk is not a regular file")
		}
		if info.Size() < factoryInfo.Size() {
			// Older launchers copied directly to disk.raw. A setup interrupted
			// during that copy left the partial file under its final name. Keep it
			// for recovery or inspection, then build a complete disk atomically.
			quarantine := fmt.Sprintf("%s.incomplete-%d", cfg.disk, time.Now().UnixNano())
			if err := os.Rename(cfg.disk, quarantine); err != nil {
				return fmt.Errorf("quarantining incomplete disk: %w", err)
			}
		} else {
			// A smaller disk can be a complete guest from an older release whose
			// expanded size has since increased. Grow it in place and let the existing
			// systemd-growfs-root unit extend ext4 at boot. Replacing it with the
			// factory image here would silently discard the user's system and files.
			disk, err := os.OpenFile(cfg.disk, os.O_RDWR, 0)
			if err != nil {
				return fmt.Errorf("opening the existing writable disk: %w", err)
			}
			if err := setSparse(disk); err != nil {
				disk.Close()
				return fmt.Errorf("marking the existing writable disk sparse: %w", err)
			}
			if err := disk.Truncate(expandedBytes); err != nil {
				disk.Close()
				return fmt.Errorf("growing the existing writable disk: %w", err)
			}
			if err := disk.Sync(); err != nil {
				disk.Close()
				return fmt.Errorf("flushing the expanded writable disk: %w", err)
			}
			if err := disk.Close(); err != nil {
				return fmt.Errorf("closing the expanded writable disk: %w", err)
			}
			return nil
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
	allocated, err := allocatedFileBytes(filepath.Join(cfg.guestDir, "rootfs.ext4"))
	if err != nil {
		return fmt.Errorf("measuring the factory disk: %w", err)
	}
	if allocated > (1<<63-1)-diskSpaceReserve {
		return fmt.Errorf("factory disk allocation is too large")
	}
	if err := requireDiskSpace(cfg.vmDir, allocated+diskSpaceReserve); err != nil {
		return fmt.Errorf("preflighting writable disk storage: %w", err)
	}
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
	if !st.Mode().IsRegular() || st.Size() > expandedBytes {
		return fmt.Errorf("factory image does not fit the requested disk capacity")
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

// preparePortableDisk publishes a compact QCOW2 overlay through a sibling
// staging file. Its backing path is relative so drive-letter changes do not
// break it, and an interrupted creation never appears under the final name.
func preparePortableDisk(cfg *config, expandedBytes int64) error {
	backing := filepath.ToSlash(filepath.Join("..", "guest", "rootfs.ext4"))
	backingSHA256, ok := installReceiptArtifactSHA256(cfg.guestDir, "rootfs.ext4")
	if !ok {
		return fmt.Errorf("verified factory disk identity is missing")
	}
	ok, err := qcow2OverlayMatches(cfg.disk, backing, expandedBytes)
	if err != nil {
		return err
	}
	if ok {
		matches, err := portableBackingStateMatches(cfg.disk, backingSHA256)
		if err != nil {
			return err
		}
		if !matches {
			return fmt.Errorf("portable guest data belongs to a different factory image; restore the matching payload or start with -portable -fresh")
		}
		return nil
	}
	if info, statErr := os.Lstat(cfg.disk); statErr == nil {
		if !info.Mode().IsRegular() {
			return fmt.Errorf("disk path is not a regular file: %s", cfg.disk)
		}
		quarantine := fmt.Sprintf("%s.incomplete-%d", cfg.disk, time.Now().UnixNano())
		if err := os.Rename(cfg.disk, quarantine); err != nil {
			return fmt.Errorf("quarantining incomplete disk: %w", err)
		}
	} else if !os.IsNotExist(statErr) {
		return statErr
	}
	tmp := cfg.disk + ".part"
	if err := os.Remove(tmp); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("removing stale disk staging file: %w", err)
	}
	if err := createQcow2Overlay(tmp, backing, expandedBytes); err != nil {
		return fmt.Errorf("creating compact USB disk: %w", err)
	}
	published := false
	defer func() {
		if !published {
			os.Remove(tmp)
		}
	}()
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	// Publish the authenticated backing identity first. If setup is interrupted
	// before the QCOW2 rename, the harmless sidecar is reused on the next
	// attempt. Publishing the disk first would leave a crash window where an
	// existing overlay had no safe way to identify its backing image.
	if err := writePortableBackingState(cfg.disk, backingSHA256); err != nil {
		return err
	}
	if err := renamePortableFileWithRetry(tmp, cfg.disk); err != nil {
		return err
	}
	published = true
	return nil
}
