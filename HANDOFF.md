# Handoff — next session: Windows laptop, validate v0.0.2-preview + the app shell

Two things shipped since the laptop last looked (both from the Linux box):

1. **v0.0.2-preview image** —
   https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.2-preview
   — Omarchy 4.0.1, all 22 themes, screensavers, permanent autologin, baked-in
   clipboard bridge, 9p automount, visible cursor. KVM-validated end to end.
2. **The native app shell** — `app/` builds ONE console-less TryOmarchy.exe
   that replaces every PowerShell script including bootstrap's download: first
   run shows a progress window, pulls the image from the release
   (SHA256-verified), unpacks, boots, supervises. Fully validated in the dev
   VM (CPU mode) including the whole first-run download flow. Build from this
   repo on any machine with Go: `cd app && GOOS=windows GOARCH=amd64 go build
   -trimpath -ldflags "-H windowsgui -s -w" -o TryOmarchy.exe .` (on the
   laptop plain `go build` works too).

**Read the new FINDINGS.md section "THE LAUNCH WEDGE, SOLVED" before touching
anything** — early QMP connections CAUSE the wedge; the shell already handles
it, but any hand-rolled QMP tooling must wait ~10s after launch.

## Shell validation on hardware (the new part, ~30 min)

Run TryOmarchy.exe (no args; `%LOCALAPPDATA%\TryOmarchy` is its home) on the
laptop and check the things the VM cannot:

- first-run download UX (window visible, progress sane, ends in the setup form)
- GPU mode auto-detect (C:\WINQ-EMU present -> virgl/Venus; log says which)
- audio actually plays (dsound path — the VM only proved the silent fallback)
- window comes to the foreground on double-click, title stays "Try Omarchy",
  Win key scoped to the window, Ctrl+Alt+F fullscreen, -fullscreen flag
- clipboard both ways, -share <folder> shows up at /mnt/host
- in-guest reboot relaunches (WINQ path delivers the event; on -nogpu stock
  QEMU it will EXIT instead — known, fixed by the v0.0.3 image's reboot-notify
  unit, patch 0004, not yet in a published image)
- poweroff from Omarchy exits the app cleanly

Log lives at %LOCALAPPDATA%\TryOmarchy\vm\shell.log; QEMU's own errors at
vm\qemu-stderr.log.

## The image validation pass (same as before, 30–45 min)

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
