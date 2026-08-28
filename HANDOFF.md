# Laptop handoff — start here

You're on the Windows laptop, picking up try-omarchy-windows. Everything below was
proven 2026-08-27 in the nested dev VM (Linux → KVM → Win11 → WHPX → Omarchy);
**nothing has run on bare-metal Windows yet — that's your job.**

## Hour one: validate bare metal (the release gate)

1. Clone this repo. Elevated PowerShell:
   `powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1`
   (enables WHP → reboot → rerun; installs QEMU 11 + zstd via winget; downloads the
   guest image from the v0.0.1-preview release)
2. `powershell -ExecutionPolicy Bypass -File scripts\launch-omarchy.ps1`
   First boot shows Omarchy's setup form in the QEMU window (keyboard, user, password,
   hostname, timezone) — complete it, land in Hyprland.
3. Record in NOTES.md: did WHPX init first try; wall-clock to setup form and to
   desktop; `systemd-analyze` in a terminal (Super+Return); does llvmpipe feel usable;
   any SDL window issues (SDL is the one piece never exercised in the dev VM —
   headless VNC was used there; SDL is proven only by jorge's fork).

If all that works, the preview release is validated — say so in NOTES.md.

## Hour two+: the Venus milestone (the differentiator)

Follow "Windows laptop bootstrap" in [NOTES.md](NOTES.md) — WINQ-EMU Alpha 10,
swap the QEMU binary and graphics device in launch-omarchy.ps1, prove Hyprland on
real GPU (vulkaninfo shows Venus, not llvmpipe). Bonus from the same patched QEMU:
try `-cpu host` — it also unlocks AVX2 llvmpipe (stock WHPX panics on any XSAVE
feature; see the XSAVE cliff in [docs/FINDINGS.md](docs/FINDINGS.md)).

## Traps that will bite you if you skip the docs

- `-vga none` is mandatory with virtio-gpu — without it you get an invisible second
  display that looks exactly like a boot hang.
- Stock QEMU + any AVX/XSAVE cpu flag = guest kernel panic at 0.25s. Launch
  acceptance proves nothing.
- Windows sshd kills the process tree when an ssh session ends — anything you start
  over ssh must be held by an open session or a scheduled task.
- The guest image has no sshd (79 packages); automation goes through QMP
  (`scripts/qmp.ps1`: shot / type / key against tcp:127.0.0.1:4445 when launched
  with the QMP flag — see start-omarchy.ps1 for the headless variant).

## State of the world

- Repo: https://github.com/tsouth89/try-omarchy-windows (public, tsouth89)
- Release: v0.0.1-preview — guest image artifacts + these scripts
- Proven: WHPX boot, full provisioning over QMP, Hyprland renders, 9.03s to
  graphical.target (nested), safe CPU pack `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes`
- Open (ordered): bare-metal validation → Venus → SDDM autologin / instant-try
  provisioning → signed bootstrapper → then a real 0.1 announcement
- Competitive picture and credits: README. Jorge's fork (jorge-huxley/try-omarchy-win)
  is llvmpipe-only and unannounced; Chainfire's repo is a manual recipe. Collab was
  publicly offered in the announcement tweet.

Keep NOTES.md updated as you go — it's the cross-machine memory.
