package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// buildQemuArgs is the argument recipe from scripts/launch-omarchy.ps1,
// unchanged: GPU mode is WINQ-EMU's stack (patched WHPX survives -cpu host;
// virtio-vga-gl IS the VGA device, so no -vga none), CPU mode is stock QEMU
// with the fastest flags upstream WHPX survives (any XSAVE/AVX feature panics
// the guest kernel) and the mandatory -vga none for virtio-gpu-pci.
func buildQemuArgs(cfg *config, cmdline string) []string {
	vm := cfg.vmDir
	args := []string{}
	if cfg.useGpu {
		args = append(args,
			"-machine", "q35,accel=whpx", "-cpu", "host", "-smp", "6", "-m", "6G",
			"-device", "virtio-vga-gl,blob=on,hostmem=4G,venus=on",
			// show-cursor=on: during console phases the guest draws no cursor
			// and a vanishing host pointer reads as broken (launch-UX contract).
			// window-close=off: the X must not hard-kill a running OS; the
			// close guard intercepts the click and confirms + shuts down
			// gracefully instead (closeguard.go).
			"-display", "sdl,gl=on,show-cursor=on,window-close=off",
			"-serial", "file:"+filepath.Join(vm, "serial-gpu.log"),
		)
	} else {
		args = append(args,
			"-machine", "q35,accel=whpx", "-cpu", "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes",
			"-smp", "6", "-m", "4096",
			"-vga", "none", "-device", "virtio-gpu-pci,id=gpu0",
			"-display", "sdl,gl=off,show-cursor=on,window-close=off",
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
		"-audiodev", cfg.audio + ",id=snd",
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

// prepareDisk gives the guest its writable disk: a sparse copy of the factory
// rootfs, extended to the spec's expanded size (the NTFS sparse-file trick from
// jorge's fork - the file reads as 24 GiB without occupying it). The copy takes
// a minute or two, so it gets the same progress window the download uses -
// launch must never look hung.
func prepareDisk(cfg *config, expandedMiB int64) error {
	if cfg.fresh {
		os.Remove(cfg.disk)
	}
	if _, err := os.Stat(cfg.disk); err == nil {
		return nil
	}
	src, err := os.Open(filepath.Join(cfg.guestDir, "rootfs.ext4"))
	if err != nil {
		return err
	}
	defer src.Close()
	dst, err := os.Create(cfg.disk)
	if err != nil {
		return err
	}
	defer dst.Close()
	if err := setSparse(dst); err != nil {
		return fmt.Errorf("marking disk sparse: %w", err)
	}
	ui := newProgressUI()
	defer ui.finish()
	ui.setStatus("Preparing your Omarchy disk...")
	st, _ := src.Stat()
	if err := sparseCopy(dst, src, st.Size(), ui); err != nil {
		os.Remove(cfg.disk)
		return err
	}
	return dst.Truncate(expandedMiB * 1024 * 1024)
}
