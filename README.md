# Try Omarchy for Windows

Run the full [Omarchy](https://omarchy.org) desktop as a native app on Windows. No VMware, no VirtualBox: QEMU on the Windows Hypervisor Platform (WHPX), a prebuilt Arch image with Omarchy baked in, and (the goal) GPU-accelerated graphics over virtio-gpu with Venus Vulkan forwarding.

Download, boot, Hyprland.

**Status: working developer preview, GPU-accelerated.** The full Omarchy desktop boots and renders under WHPX-accelerated QEMU on Windows — on the real GPU (virgl + Venus Vulkan via [WINQ-EMU](https://github.com/cmspam/winq-emu)) when it's installed, with CPU rendering (llvmpipe) as the automatic fallback.

## What works today

- Omarchy (Hyprland, bar, notifications, the whole desktop) boots to a rendered session under WHPX on Windows 11
- **GPU acceleration**: Hyprland renders on the host GPU via virgl, `vulkaninfo` shows Venus, smooth video + audio (verified on a Radeon iGPU laptop); `-cpu host` (AVX2 and all) via WINQ-EMU's patched WHPX
- One supervised launcher: auto-detects GPU vs CPU mode, retries the known WHPX launch wedge, cleans up the guest-poweroff wedge, relaunches on in-guest reboot, and never shows a console window
- App-shell prototype: the Windows key acts as Super only while the VM window is focused (Start menu and Win+Shift+S work normally otherwise), and the window is branded "Try Omarchy", not "QEMU"
- First-boot account provisioning driven programmatically over QMP (basis for a "skip setup, just try it" mode), SDDM autologin after setup
- Reproducible x86_64 guest image build (containerized, package-locked, pinned Omarchy revision)
- Headless control plane: QMP screendump/send-key scripting for automated testing

## Architecture

Same recipe as the excellent macOS [try-omarchy](https://github.com/themartiano/try-omarchy) (QEMU + Apple Hypervisor Framework + VirGL), translated to Windows:

| Piece | macOS (try-omarchy) | This project |
|---|---|---|
| Hypervisor | Hypervisor.framework | Windows Hypervisor Platform (WHPX) |
| Guest image | ARM64 Arch + Omarchy | x86_64 Arch + Omarchy |
| Graphics | VirGL | virtio-gpu virgl + Venus Vulkan (WINQ-EMU); llvmpipe fallback |
| App shell | Swift/AppKit | PowerShell prototype today (supervised launcher + focus-scoped Win-key forwarder); native shell planned |

WHPX works on Windows Home and Pro (it's the same platform WSL2 rides on), so no Hyper-V role is required.

Proven boot recipe: `-accel whpx -machine q35 -cpu qemu64`, direct kernel boot (vmlinuz + initramfs + raw ext4 rootfs on virtio-blk), all-virtio devices, DirectSound audio. See [docs/FINDINGS.md](docs/FINDINGS.md) for the details and the traps.

## Try it (developer preview)

For tinkerers comfortable with PowerShell — this is not the polished app yet. From an elevated PowerShell:

```powershell
git clone https://github.com/tsouth89/try-omarchy-windows
cd try-omarchy-windows
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1   # WHP + QEMU + image; may ask for one reboot
powershell -ExecutionPolicy Bypass -File scripts\launch-omarchy.ps1
```

First boot walks you through Omarchy's setup form, then you're in Hyprland; later boots log you straight in.

For GPU acceleration, install [WINQ-EMU Alpha 10](https://github.com/cmspam/winq-emu/releases) to `C:\WINQ-EMU` first — the launcher detects it and switches to the virgl/Venus stack automatically (pass `-NoGpu` to force CPU rendering). Without it you get llvmpipe with the fastest CPU flags stock WHPX survives.

## Repository layout

- `scripts/` — PowerShell scripts that boot and drive the guest (QMP screendump, send-key typing, WHPX smoke test)
- `docs/FINDINGS.md` — technical findings, gotchas, and their fixes
- `NOTES.md` — live working notes / session handoff

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s `win` branch guest builder (`guest/build-container.sh`, needs Docker on Linux) — an x86_64 retarget of the upstream try-omarchy build system. Images are not committed; build one or grab a release artifact once releases exist.

## Credit where due

This project stands on a lot of shoulders:

- [Omarchy](https://github.com/basecamp/omarchy) by DHH / Basecamp — the desktop this is all about
- [try-omarchy](https://github.com/themartiano/try-omarchy) by Eduardo (themartiano) — the original macOS app and the architecture this follows
- [try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win) by Jorge Silva — the x86_64 guest builder retarget and the proven WHPX boot recipe this project reuses
- [WINQ-EMU](https://github.com/cmspam/winq-emu) by cmspam — Venus Vulkan GPU forwarding for QEMU on Windows, the planned graphics path
- [omarchy-windows-hyperv-gpu](https://github.com/Chainfire/omarchy-windows-hyperv-gpu) by Chainfire — prior art proving GPU-accelerated Omarchy on Windows, plus the QEMU 11 WHPX interrupt findings
- [dockur/windows](https://github.com/dockur/windows) — the Windows-in-Docker environment this is developed and tested in

Open to collaboration — if you're working on any of this, get in touch.

## License

Scripts and docs in this repo: [MIT](LICENSE). Omarchy and the guest image contents carry their own licenses.
