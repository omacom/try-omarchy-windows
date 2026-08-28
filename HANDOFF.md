# Handoff — next session: Windows laptop, validate v0.0.2-preview

The image rebuild is done and published (2026-08-28 evening, Linux box):
**https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.2-preview**
— Omarchy 4.0.1, all 22 themes, screensavers, permanent autologin, baked-in
clipboard bridge, 9p automount, visible cursor. Validated end to end under KVM
(form → desktop → reboot → desktop, no greeter). What's left is proving the same
image on real Windows hardware with the real launcher.

## The validation pass (30–45 min)

1. `git pull` this repo (launcher + bootstrap changed too).
2. Get the new image. Either delete `%LOCALAPPDATA%\TryOmarchy\guest` and rerun
   `scripts\bootstrap.ps1` (it now points at v0.0.2-preview), or download the
   four artifacts manually into `guest\`. **Then launch with `-Fresh`** — the
   laptop's current disk has everything hand-installed and will mask image bugs.
3. `powershell -ExecutionPolicy Bypass -File scripts\launch-omarchy.ps1 -Fresh`
   Walk the setup form. Confirm, in order:
   - boot is a black window until the branded splash (no console text, no
     blinking cursor — the launcher now drops console=tty0/tty1 from the
     cmdline; boot logs are in `vm\serial*.log` only)
   - cursor visible in the SDL window without any in-guest fix
   - `omarchy-version` says 4.0.1-1; theme picker shows 22 themes
   - screensaver runs (idle or `omarchy-launch-screensaver`)
   - clipboard both directions with zero setup (bridge is baked in + host side
     auto-starts)
   - `-Share <folder>` appears at `/mnt/host` with no manual mount (GPU mode)
   - reboot from inside Omarchy → relaunches → straight back to the desktop
     (permanent autologin; the generic SDDM greeter must never appear)
4. Record results + timings in NOTES.md. If it all passes, v0.0.2-preview is
   the validated preview and the old caveats in README can be tightened.

## Known state / gotchas

- The published image still has the noisy push-side socat in the clipboard
  bridge (guest-build patch 0003 landed after the build). Harmless — the
  launcher always runs the host listener. Rolls into the next image.
- Stock-QEMU (CPU/llvmpipe) mode keeps its known wedges; the supervisor handles
  them. GPU mode (WINQ-EMU at C:\WINQ-EMU) is the real product path.
- All the deep traps live in docs/FINDINGS.md (XSAVE cliff, -vga none, sshd
  process-tree kills, poweroff wedge, QMP-vs-GL screendump).
- Image builds now happen on the Linux box with `guest-build/*.patch` on top of
  jorge's builder (guest-build/README.md has the exact commands). The Linux box
  can also KVM-validate an image without any Windows VM — see the 2026-08-28
  evening session log in NOTES.md.

## After validation (rough order)

- Pre-provisioned "just try it" image variant (skip the form entirely)
- Native app shell (window embedding, branded icon, WHP-enable installer flow)
- Signed bootstrapper / packaging decision
- Outreach to Eduardo (themartiano) and Jorge (jorge-huxley); consider
  upstreaming the -vga none finding and the 4.0.1 pin bump to jorge's fork
