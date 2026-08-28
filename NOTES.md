# try-omarchy-windows — working notes / session handoff

**Read this first in a new session.** Keep this file updated as work progresses.
Last updated: 2026-08-27 (bare-metal laptop bootstrap in progress).

## Bare-metal validation (Windows laptop, 2026-08-27)

Hardware: AMD Ryzen 5 5625U (Zen 3, 6C/12T, AVX2), Radeon integrated GPU (Vulkan-capable
— Venus candidate), 16 GB RAM, Windows 11 Pro 26200.

Bootstrap status:
- [x] Repo cloned to `C:\cssi\try-omarchy-windows`
- [x] WHP optional feature enabled (needed one host reboot; hypervisor was already
      running via HvHost/vmcompute, but the HypervisorPlatform feature itself was off)
- [x] QEMU 11.1.0 installed via winget (qemu-w64-setup-20260811)
- [x] zstd 1.5.7 installed — **bootstrap.ps1 bug found+fixed:** winget id
      `Facebook.Zstandard` no longer exists, renamed `Meta.Zstandard`; also the
      `WinGet\Links\zstd.exe` shim didn't materialize in-session, so the script now
      falls back to searching WinGet Packages
- [x] v0.0.1-preview guest artifacts downloaded, rootfs decompressed (6 GiB)

**VALIDATED 2026-08-27 evening.** Results (details in FINDINGS.md):
- WHPX initialized first try; full boot → setup form → Hyprland desktop in the SDL
  window. Setup form driven 100% over QMP from the host (user brandon / hostname
  omarchy / America/Chicago).
- Clean provisioned boot: **3.22s kernel + 3.54s user = 6.8s to graphical.target**
  (vs 9.03s nested). ~40s launch→setup-splash on first boot incl. one-time disk copy.
- Second boot lands on SDDM login as expected — autologin/seamless-login remains the
  top UX gap for instant-try.
- **New trap:** in-guest `systemctl reboot` wedges QEMU (WHPX can't do system reset).
  `-no-reboot` added to launch-omarchy.ps1; app shell must relaunch on exit. See
  FINDINGS.md "NEW TRAP".
- llvmpipe subjective feel (Brandon, bare metal): desktop is workable, but media is
  not — YouTube video playback is poor and audio crackles (dsound + llvmpipe
  starving the audio thread on 4 vCPUs). Confirms Venus GPU path as the priority.
  To try at next relaunch: -smp 6, dsound latency= tuning or -audiodev sdl.
- SDL UX (see FINDINGS.md "SDL desktop UX findings"): cursor was invisible because
  the image's monitors.lua sets cursor invisible=true (correct for VNC hosts, wrong
  for SDL) — flipped to false in this VM, **must be fixed in the image builder**;
  the image uses Hyprland 0.56 Lua configs (edit *.lua + hyprctl reload; `hyprctl
  keyword` doesn't work); window resize tracks fine; Windows key fights the host
  Start menu unless input is grabbed (Ctrl+Alt+G) — app shell needs a keyboard hook.

## Current state

- Milestone reached 2026-08-27: full Omarchy desktop (Hyprland on llvmpipe) boots and
  renders under WHPX QEMU inside the dev VM. Parity with jorge-huxley's fork.
- Announced on X (quote-tweet of DHH, 2026-08-27), offering to collab. The stated
  differentiator vs jorge's fork: GPU acceleration via virtio-gpu Venus Vulkan
  (their spec self-declares llvmpipe-only), plus a slicker zero-setup UX.
- Read `docs/FINDINGS.md` for everything proven so far and the traps (especially
  the `-vga none` display trap and the WHPX CPU-model constraint).

## Environments

**Linux desktop (primary dev, this machine):** guest image builds (Docker), and a
dockur/windows Win11 VM used as the Windows test bed (nested KVM → WHPX works).
Fork checkout with the guest builder: `~/Projects/try-omarchy-win` (win branch).
Working scripts + built image shared into the VM at `~/Windows/tryomarchy/`.
Machine-specific access details (VM ssh, ports) live outside this repo.

**Windows laptop (needed for GPU work):** nothing set up yet. Bootstrap below.

## Windows laptop bootstrap (the GPU/Venus milestone)

Goal: prove GPU-accelerated Omarchy via WINQ-EMU's Venus Vulkan path on real hardware.

1. Clone this repo. Read `docs/FINDINGS.md`.
2. Enable Windows Hypervisor Platform: elevated PowerShell,
   `Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All`, reboot.
   (Windows Home or Pro both fine.)
3. Install [WINQ-EMU Alpha 10](https://github.com/cmspam/winq-emu/releases) — exactly
   Alpha 10 if following Chainfire's recipe; check his repo
   (Chainfire/omarchy-windows-hyperv-gpu) for the interrupt patch and whether it
   applies (it targets Windows-on-Hyper-V hosts; a bare-metal laptop may not need it).
   Note WINQ-EMU wants BIOS boot, not EFI — our direct-kernel-boot image sidesteps that.
4. Get a guest image: build on the Linux box (`try-omarchy-win/guest/build-container.sh`,
   refresh the package lock first if Arch moved) and transfer `dist/guest/`
   (use rootfs.ext4.zst, ~1.2G, plus vmlinuz/initramfs/build-spec.json), or rebuild
   fresh if Docker is available.
5. Adapt `scripts/start-omarchy.ps1`: swap the QEMU path for WINQ-EMU's binary, replace
   `-vga none -device virtio-gpu-pci,id=gpu0` + VNC display with WINQ-EMU's
   virtio-gpu-gl/Venus device + SDL display config (see their launcher/docs), keep the
   rest of the recipe. First run headless smoke (`whpx-boot-test.ps1` pattern), then SDL.
6. Success criteria: Hyprland renders with `vulkaninfo`/`glxinfo` in-guest showing Venus
   (not llvmpipe), and animations/blur feel smooth. Grab numbers + screenshots, update
   this file and FINDINGS.md.

## Open work, in rough order

- [x] CPU-feature experiment DONE 2026-08-27: XSAVE/AVX panics guests under upstream
      WHPX; safe ceiling `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes` (now default in
      scripts/start-omarchy.ps1). AVX2 needs WINQ-EMU's patched WHPX. See FINDINGS.md.
- [x] Boot profile baseline 2026-08-27: 9.03s to graphical.target in the nested dev
      VM (5.1s kernel + 4.0s user). Trim + SDDM autologin still open.
- [ ] Seamless/auto login: post-provisioning boots land on SDDM login; instant-try
      UX needs autologin (or upstream omarchy seamless-login) wired into the image.
- [ ] "Just try it" mode: auto-provision a default account over QMP (or pre-provisioned
      image variant) so first boot lands straight in Hyprland.
- [ ] Dev-image variant with openssh + QEMU hostfwd for real in-guest automation
      (current image is 79 packages, no sshd; everything goes through QMP send-key).
- [ ] Venus/WINQ-EMU on the laptop (see bootstrap above) — the differentiator.
- [ ] Native app shell design: window embedding vs own display client (QMP/VNC/D3D
      surface), lifecycle, sparse-disk management, WHP-enable installer flow.
- [ ] Reach out to Eduardo (themartiano) and Jorge (jorge-huxley) re: collab, and
      consider upstreaming the `-vga none` finding to jorge's fork.

## Done

- 2026-08-27: WHPX proven in dev VM (Alpine boot), Omarchy x86_64 image built (lock
  refresh needed), full provisioning driven over QMP, Hyprland rendering confirmed.
  Repo created, scripts and findings captured.
