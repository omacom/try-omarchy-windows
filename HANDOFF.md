# Handoff — start here

**The original laptop handoff is complete**: bare-metal validation passed and the
Venus GPU milestone was hit 2026-08-27, both on the Ryzen 5 5625U laptop. Current
working state, traps, and the release plan live in [NOTES.md](NOTES.md); everything
proven (and everything that bit us) is in [docs/FINDINGS.md](docs/FINDINGS.md).

## Quick start on a fresh Windows machine

1. Elevated PowerShell (first time on a machine only — later runs don't need admin):
   `powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1`
   (enables WHP → reboot → rerun; installs QEMU 11 + zstd via winget; downloads the
   guest image from the v0.0.1-preview release)
2. Optional but recommended: install WINQ-EMU Alpha 10 to `C:\WINQ-EMU` for GPU
   acceleration (https://github.com/cmspam/winq-emu/releases).
3. `powershell -ExecutionPolicy Bypass -File scripts\launch-omarchy.ps1`
   First boot shows Omarchy's setup form; after that, autologin straight to Hyprland.
   The launcher supervises QEMU: retries the WHPX launch wedge, reaps the poweroff
   wedge, relaunches on in-guest reboot, hides all consoles, keeps the window
   titled "Try Omarchy", and scopes the Windows key to the VM window.

## The traps, if you touch the QEMU recipe

- `-vga none` is mandatory with `virtio-gpu-pci` (invisible-second-display trap).
  Not needed with WINQ-EMU's `virtio-vga-gl` — that IS the VGA device.
- Stock QEMU + any AVX/XSAVE cpu flag = guest kernel panic at 0.25s. `-cpu host`
  needs WINQ-EMU's patched WHPX.
- Guest reboot/poweroff wedges stock WHPX QEMU — and a wedged poweroff can LOSE
  recent guest writes (`sync` first). The launcher handles both.
- QMP screendump doesn't work on the GL path ("no surface") — drive/verify via the
  serial console (`cmd | sudo tee /dev/ttyS0` lands in the host-side serial log).
- Windows sshd kills the process tree when an ssh session ends.
- The guest image has no sshd; automation goes through QMP (`scripts/qmp.ps1`,
  tools port 4445; 4446 is the winkey-forwarder's, 4447 the supervisor's).

## State of the world

- Repo: https://github.com/tsouth89/try-omarchy-windows (public, tsouth89)
- Release: v0.0.1-preview — guest image artifacts + scripts (script polish since)
- Proven: WHPX boot, QMP provisioning, Hyprland on Venus/virgl (GPU) and llvmpipe
  (CPU), 6.8s to graphical.target bare metal, SDDM autologin, focus-scoped Win key
- Next: image rebuild with the fixes list in NOTES.md, then a v0.0.2 preview and
  the native app shell. Collab offered publicly to Eduardo (themartiano) and
  Jorge (jorge-huxley); competitive picture and credits in README.
