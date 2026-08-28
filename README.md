# Try Omarchy for Windows

Run the full [Omarchy](https://omarchy.org) desktop as a native app on Windows. No VMware, no VirtualBox: QEMU on the Windows Hypervisor Platform (WHPX), a prebuilt Arch image with Omarchy baked in, and (the goal) GPU-accelerated graphics over virtio-gpu with Venus Vulkan forwarding.

Download, boot, Hyprland.

**Status: working proof of concept.** The full Omarchy desktop boots and renders under WHPX-accelerated QEMU on Windows. Graphics currently render on CPU (llvmpipe); the Venus GPU path is the next major milestone.

## What works today

- Omarchy (Hyprland, bar, notifications, the whole desktop) boots to a rendered session under `qemu-system-x86_64 -accel whpx` on Windows 11
- First-boot account provisioning driven programmatically over QMP (basis for a "skip setup, just try it" mode)
- Reproducible x86_64 guest image build (containerized, package-locked, pinned Omarchy revision)
- Headless control plane: QMP screendump/send-key scripting for automated testing

## Architecture

Same recipe as the excellent macOS [try-omarchy](https://github.com/themartiano/try-omarchy) (QEMU + Apple Hypervisor Framework + VirGL), translated to Windows:

| Piece | macOS (try-omarchy) | This project |
|---|---|---|
| Hypervisor | Hypervisor.framework | Windows Hypervisor Platform (WHPX) |
| Guest image | ARM64 Arch + Omarchy | x86_64 Arch + Omarchy |
| Graphics | VirGL | llvmpipe today; virtio-gpu + Venus Vulkan planned |
| App shell | Swift/AppKit | TBD (native Windows) |

WHPX works on Windows Home and Pro (it's the same platform WSL2 rides on), so no Hyper-V role is required.

Proven boot recipe: `-accel whpx -machine q35 -cpu qemu64`, direct kernel boot (vmlinuz + initramfs + raw ext4 rootfs on virtio-blk), all-virtio devices, DirectSound audio. See [docs/FINDINGS.md](docs/FINDINGS.md) for the details and the traps.

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
