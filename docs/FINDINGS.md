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

## WHPX CPU features: the XSAVE cliff (2026-08-27 evening)

Upstream QEMU WHPX accepts any `-cpu` model at launch, but **any XSAVE-state feature
(AVX and above) panics the guest kernel at ~0.25s in `fpstate_reset`** — upstream WHPX
does not virtualize XSAVE/XCR0 state correctly. Launch acceptance means nothing;
Skylake-Client-v3, Haswell-v4, and `qemu64,+avx,...` all pass validation and then
panic the guest.

- Safe proven ceiling: `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes` — boots the full
  Omarchy image cleanly, flags visible in-guest. llvmpipe benefits from SSE4.1/4.2,
  so ship this as the fallback-tier CPU model instead of bare qemu64.
- AVX2-accelerated llvmpipe therefore REQUIRES WINQ-EMU's patched WHPX (their
  "full -cpu host passthrough" claim is exactly this fix). WINQ-EMU matters for the
  CPU-rendering fallback, not just Venus.
- Methodology traps, learned the hard way: (1) a panicked guest leaves the QEMU
  process running — check guest health, not process liveness; (2) Windows OpenSSH
  kills the session's process tree on disconnect, so QEMU must not outlive the ssh
  session that started it (hold the session or use a scheduled task); (3) Alpine's
  `virt` kernel has no virtio-gpu driver — use `-device VGA` for Alpine-based text
  harnesses; the Omarchy image has virtio-gpu and is fine.

## Bare-metal validation (2026-08-27, Ryzen 5 5625U laptop, Windows 11 Pro)

The v0.0.1-preview flow works on real hardware. WHPX initialized first try (QEMU
11.1.0 prints a benign `warning: Ignoring request for interrupt vector 0` at start).
First boot: ~40s from launch to the setup splash (includes the one-time 6 GiB sparse
disk copy); the entire gum form was driven over QMP from the host — the provisioning
automation works on bare metal exactly as in the dev VM. Second (provisioned) boot:
**3.22s kernel + 3.54s userspace = 6.8s to graphical.target** (vs 9.03s nested), then
SDDM login → full desktop with wallpaper. SDL display worked throughout (screendumps
via QMP match what the window shows; no SDL-specific issues observed).

### SDL desktop UX findings (bare metal)

- **Invisible mouse cursor:** Hyprland puts its cursor on the virtio-gpu hardware
  cursor plane, which QEMU's Windows SDL display never renders. Fix in-guest:
  `cursor:no_hardware_cursors = true` in `~/.config/hypr/hyprland.conf` (flat
  `category:option` syntax — `hyprctl keyword` is rejected by current Hyprland's
  parser, so append to the config and `hyprctl reload`). Hyprland then composites
  the cursor into the frame. **Bake this into the guest image.**
- **Window resize works:** virtio-gpu propagates SDL window resizes; Hyprland adapts
  its resolution live. Transient cropping can appear until the next resize event.
- **Windows key collision:** Super is Omarchy's main modifier, but the host swallows
  it (Start menu opens) unless QEMU has grabbed input. Ctrl+Alt+G (SDL grab) makes
  SDL install the low-level keyboard hook that swallows Win-key on the host;
  fullscreen also helps. The future app shell must do the same (keyboard grab /
  low-level hook while the guest window is focused).

### NEW TRAP: in-guest reboot wedges QEMU under WHPX

`systemctl reboot` inside the guest hangs the whole VM at the reset: the guest shuts
down cleanly, issues the reset, and never comes back — SDL window freezes on black,
serial stops, QMP stops answering, and the QEMU process sits alive at ~0% CPU.
Upstream WHPX apparently cannot execute a system reset (same class of gap as the
XSAVE cliff). Also observed: after force-killing the wedged process, the *next*
launch froze at ~1.4s into kernel boot (possibly leaked WHPX partition state);
killing that one and launching again booted clean in ~20s.

Mitigation: launch with `-no-reboot` so a guest-initiated reset exits QEMU instead of
wedging it (now in launch-omarchy.ps1). The app shell must treat guest reboot as
"QEMU exits → relaunch", e.g. via `-action reboot=shutdown` + QMP RESET/SHUTDOWN
events if it needs to distinguish reboot from poweroff.

## Boot profile (nested dev VM, SSE4.2 pack)

`systemd-analyze` in the Omarchy guest: **5.06s kernel + 3.97s userspace = 9.03s to
graphical.target**, then SDDM. Top blame: dev-vda.device 1.66s,
systemd-tmpfiles-setup-dev-early 1.14s, user-runtime-dir 1.10s, systemd-userdbd 1.06s.
Bare metal will be faster (this carries KVM-nesting overhead). Note: after first-boot
provisioning auto-login, subsequent boots land on SDDM login — a seamless-login or
autologin config is needed for the "instant try" UX.
