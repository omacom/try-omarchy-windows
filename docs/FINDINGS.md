# Technical findings

Working notes on what's proven, what bit us, and the fixes. Dates are 2026-08.

## WHPX boot recipe (proven)

```
qemu-system-x86_64 -accel whpx -machine q35 -cpu qemu64 -smp 4 -m 4096
  -drive file=disk.raw,format=raw,if=virtio
  -kernel vmlinuz-linux -initrd initramfs-linux.img -append "<cmdline>"
  -vga none -device virtio-gpu-pci,id=gpu0
  -device virtio-keyboard-pci -device virtio-tablet-pci
  -device virtio-net-pci,netdev=n0 -netdev user,id=n0
  -device virtio-rng-pci
  -display vnc=127.0.0.1:7
  -qmp tcp:127.0.0.1:4445,server=on,wait=off
  -serial file:serial.log
```

- `-cpu qemu64`, not `-cpu host`: upstream QEMU's WHPX backend rejects host passthrough (WINQ-EMU patches this). qemu64 is a 2006-era feature set — no AVX2, which hurts llvmpipe badly. Testing richer named models / feature flags is an open task.
- QEMU 11 matters: Chainfire documented WHPX interrupt fixes landing in QEMU 11 (and a startup regression on Hyper-V-host setups that WINQ-EMU Alpha 10 + his patch work around). Stock QEMU 11.1.0 from winget works fine under WHPX-on-KVM-nested and should work on bare-metal Windows.
- Direct kernel boot (no bootloader) + raw ext4 rootfs on virtio-blk, from the try-omarchy build system.
- Writable disk on NTFS: copy rootfs.ext4 to disk.raw, `fsutil sparse setflag`, then extend to the spec's expanded size via SetLength. Allocate-on-write; hosts without 24 GiB spare still boot. (Credit: jorge-huxley.)

## THE display trap: default VGA + virtio-gpu (cost us an hour)

`-device virtio-gpu-pci` does **not** suppress QEMU's default VGA. You silently get two display devices. The guest kernel's fbcon/DRM moves to the virtio-gpu, while QMP `screendump` (and anything watching the default console) keeps showing the **stale VGA text buffer** — frozen at early boot messages. It looks exactly like a boot hang. Keystrokes go through and land on the invisible display.

Fix: always pass `-vga none` alongside `-device virtio-gpu-pci`. Prefer `-display vnc=127.0.0.1:N` over `-display none` for headless work so the display pipeline stays live.

Any embedded/headless display client in the app must account for this.

## First-boot provisioning

The factory image arms upstream Omarchy's `omarchy-provision-owner.service`: an interactive gum form on tty1 (keyboard, username, password, optional name/email, hostname, timezone), run before display-manager. Completing it creates the user and hands off to SDDM → Hyprland.

The entire form is drivable over QMP `send-key` (see `scripts/qmp.ps1` — `type`/`key`/`shot` ops). This is the basis for an app-driven setup UX, or a "just try it" mode that provisions a default account with no questions.

## Guest image build

Built with the containerized x86_64 builder from jorge-huxley/try-omarchy-win (`guest/build-container.sh`, Docker on Linux, ~10 min). Two operational notes:

- The build enforces `packages.lock.json` against live Arch repos and refuses on drift. Refresh with `guest/build-container.sh --refresh-package-lock guest/packages.lock.json`, review the diff, rebuild. Expect this routinely — Arch moves fast.
- The build spec self-documents the graphics state: `guestRenderer: llvmpipe, hostRenderer: none`. The trimmed 79-package guest has no sshd; all in-guest automation goes through QMP keystrokes. A dev-image variant with openssh (+ QEMU `hostfwd`) would make benchmarking much nicer.

## Dev environment: nested virtualization under dockur/windows

Development runs inside a dockur/windows Win11 VM on a Linux/KVM host (so: Linux → KVM → Windows 11 → WHPX → Omarchy). Findings:

- dockur **masks VMX by default for Windows guests** (`-cpu ...,-vmx`, guarding an old Windows-update crash). Set the `VMX: "Y"` env on the container to expose nested virt; current Win11 builds run fine with it.
- After exposing VMX, enable the `HypervisorPlatform` optional feature in the Windows guest + reboot; then `-accel whpx` initializes. ("WHPX: No accelerator found" = the hypervisor isn't launching — check VMX exposure first.)
- The same enable-WHP + reboot flow is what the product installer must automate on end-user machines.

Caveat: all performance numbers measured in this nested environment carry KVM-nesting overhead. Relative comparisons (flag A vs flag B) are valid; absolute UX judgments need bare-metal Windows.

## GPU path (planned, needs real hardware)

WINQ-EMU proves Venus Vulkan forwarding works on Windows QEMU (their benchmark: 410 fps SuperTuxKart vs 226 under WSL2), with virgl for GL and experimental DXVA video decode. Their caveat: BIOS boot, not EFI (EFI boot tanks Vulkan perf). The dev VM has no GPU to forward, so this work needs a physical Windows machine.
