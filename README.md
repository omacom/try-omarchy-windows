# Try Omarchy for Windows

Run the full [Omarchy](https://omarchy.org) desktop in a window on Windows 11. No VMware, no VirtualBox, no dual boot: QEMU on the Windows Hypervisor Platform (WHPX), a prebuilt Arch image with Omarchy baked in, and the desktop rendered on your actual GPU (virgl + Venus Vulkan via [WINQ-EMU](https://github.com/cmspam/winq-emu)) with CPU rendering as the automatic fallback. Nothing touches your disk.

Download, boot, Hyprland.

![The Omarchy desktop running in the Try Omarchy window on Windows 11](docs/images/hero.jpg)

**Status: hardware-validated developer preview.** The whole flow is working end to end on a real laptop: one app downloads the image, boots, and supervises; the desktop renders on the GPU when [WINQ-EMU](https://github.com/cmspam/winq-emu) is installed and falls back to CPU rendering automatically. Landing page: [tryomarchy.com](https://tryomarchy.com).

## What works today

- **The full Omarchy 4.0.1 desktop**: Hyprland, the bar, notifications, all 22 themes, the screensavers. On our mid-range Ryzen 5 test laptop the desktop is up about 6 seconds after launch, and every launch after setup goes straight there. No Linux login screens, no console text, branded window.
- **GPU acceleration**: Hyprland renders on the host GPU via virgl, `vulkaninfo` shows Venus, smooth video and audio (verified on a Radeon iGPU laptop); `-cpu host` (AVX2 and all) via WINQ-EMU's patched WHPX.
- **One app**: `TryOmarchy.exe` (7 MB, no console window). First run shows a splash, downloads the image SHA256-verified (about 66 s), and boots into Omarchy's setup form. After that it supervises everything: GPU/CPU auto-detect, the known WHPX launch wedge, in-guest reboot relaunch, poweroff cleanup.
- **Feels like an app, not a VM**: the window is branded "Try Omarchy", the Windows key acts as Super only while the window is focused (Start menu and Win+Shift+S keep working everywhere else), Ctrl+Alt+F goes fullscreen.
- **Two-way clipboard sharing** between Windows and Omarchy (own compositor-native bridge over wl-clipboard, no SPICE) and **folder sharing** (`-Share <folder>`, virtio-9p on the GPU stack).
- First-boot account provisioning driven programmatically over QMP (basis for a "skip setup, just try it" mode), SDDM autologin after setup.
- Reproducible x86_64 guest image build (containerized, package-locked, pinned Omarchy revision) and a headless QMP control plane for automated testing.

| First run | Screensaver |
|---|---|
| ![Omarchy first-run setup inside the Try Omarchy window](docs/images/first-run.jpg) | ![Omarchy pixel-logo screensaver](docs/images/screensaver.jpg) |

## Architecture

Same recipe as the excellent macOS [try-omarchy](https://github.com/themartiano/try-omarchy) (QEMU + Apple Hypervisor Framework + VirGL), translated to Windows:

| Piece | macOS (try-omarchy) | This project |
|---|---|---|
| Hypervisor | Hypervisor.framework | Windows Hypervisor Platform (WHPX) |
| Guest image | ARM64 Arch + Omarchy | x86_64 Arch + Omarchy |
| Graphics | VirGL | virtio-gpu virgl + Venus Vulkan (WINQ-EMU); llvmpipe fallback |
| App shell | Swift/AppKit | Go: one console-less `TryOmarchy.exe` (PowerShell scripts remain as a fallback path) |

WHPX works on Windows Home and Pro (it's the same platform WSL2 rides on), so no Hyper-V role is required. If WSL2 runs on your machine, you're set.

Proven boot recipe: `-accel whpx -machine q35 -cpu qemu64`, direct kernel boot (vmlinuz + initramfs + raw ext4 rootfs on virtio-blk), all-virtio devices, DirectSound audio. See [docs/FINDINGS.md](docs/FINDINGS.md) for the details and the traps.

## Try it (developer preview)

The packaged single-exe download isn't ready yet (signing is in progress), so today you either build the app yourself or use the PowerShell path. Both pull the current image from the [v0.0.3-preview release](https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.3-preview) (~1.4 GB during setup).

Build the app (any machine with Go, then run the exe on Windows):

```
git clone https://github.com/tsouth89/try-omarchy-windows
cd try-omarchy-windows/app
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o TryOmarchy.exe .
```

Double-click `TryOmarchy.exe`: first run downloads the image, shows progress, walks you through Omarchy's setup form, and lands you in Hyprland. Later launches go straight to the desktop.

Or the PowerShell path (no Go needed), from an elevated PowerShell:

```powershell
git clone https://github.com/tsouth89/try-omarchy-windows
cd try-omarchy-windows
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1   # WHP + QEMU + image; may ask for one reboot
powershell -ExecutionPolicy Bypass -File scripts\launch-omarchy.ps1
```

For GPU acceleration, install [WINQ-EMU Alpha 10](https://github.com/cmspam/winq-emu/releases) to `C:\WINQ-EMU` first: the app and the launcher both detect it and switch to the virgl/Venus stack automatically. Without it you get CPU rendering with the fastest CPU flags stock WHPX survives.

## Repository layout

- `app/` — the app itself: one Go exe covering the launcher, supervisor, first-run download, focus-scoped Win-key forwarding, and the host side of the clipboard bridge
- `scripts/` — PowerShell path plus QMP tooling (screendump, send-key, WHPX smoke test)
- `guest-build/` — patches on jorge's guest builder that produce our image, plus build instructions
- `docs/FINDINGS.md` — technical findings, gotchas, and their fixes
- `NOTES.md` — live working notes / session handoff

The guest image (Omarchy 4.0.1, all upstream themes, screensavers, autologin, clipboard bridge) is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s `win` branch guest builder (`guest/build-container.sh`, needs Docker on Linux) — an x86_64 retarget of the upstream try-omarchy build system — with the patches in `guest-build/` applied. Images are not committed; setup downloads the latest release artifact, or build your own.

## Credit where due

This project stands on a lot of shoulders:

- [Omarchy](https://github.com/basecamp/omarchy) by DHH / Basecamp — the desktop this is all about
- [try-omarchy](https://github.com/themartiano/try-omarchy) by Eduardo (themartiano) — the original macOS app and the architecture this follows
- [try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win) by Jorge Silva — the x86_64 guest builder retarget and the proven WHPX boot recipe this project reuses
- [WINQ-EMU](https://github.com/cmspam/winq-emu) by cmspam — Venus Vulkan GPU forwarding for QEMU on Windows, the graphics path
- [omarchy-windows-hyperv-gpu](https://github.com/Chainfire/omarchy-windows-hyperv-gpu) by Chainfire — prior art proving GPU-accelerated Omarchy on Windows, plus the QEMU 11 WHPX interrupt findings
- [dockur/windows](https://github.com/dockur/windows) — the Windows-in-Docker environment this is developed and tested in

Open to collaboration — if you're working on any of this, get in touch.

## License

Scripts and docs in this repo: [MIT](LICENSE). Omarchy and the guest image contents carry their own licenses.
