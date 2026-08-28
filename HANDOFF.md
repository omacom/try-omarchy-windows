# Handoff — next session: Linux box, image rebuild (track 2)

The Windows side is done and pushed (see "State of the world" below). What remains
for v0.0.2-preview is the **guest image rebuild**, and that only happens on the
Linux desktop (Docker + the guest builder). This file is the brief for that session.

## Where things are on the Linux box

- Fork checkout with the guest builder: `~/Projects/try-omarchy-win` (**win** branch)
  — jorge-huxley's x86_64 retarget of the try-omarchy build system.
- Builder: `guest/build-container.sh` (Docker, ~10 min). Output: `dist/guest/`
  (vmlinuz-linux, initramfs-linux.img, build-spec.json, rootfs.ext4[.zst]).
- The build enforces `packages.lock.json` against live Arch repos and refuses on
  drift: `guest/build-container.sh --refresh-package-lock guest/packages.lock.json`,
  review the diff, rebuild. Expect to need this.
- Optional test bed: the dockur/windows Win11 VM (nested KVM → WHPX works; set
  `VMX: "Y"` on the container). Working scripts land in `~/Windows/tryomarchy/`.
  Final validation should happen on the laptop anyway.

## The image change list (all proven live in the laptop VM on 2026-08-28)

1. **Bump the Omarchy pin to 4.0.1** — image currently ships 4.0.0.alpha-1
   (hyprland 0.56.2, kernel 7.1.9). This is the one place we trail the macOS app.
2. **Add packages**: `ttfx`, `hypridle` (screensavers — required, ttfx is in the
   image's own omarchy pacman repo), `vulkan-virtio` (Venus ICD), `wl-clipboard`
   (clipboard bridge). `socat`, `foot`, `jq` are already in. Add `vulkan-tools`
   only to a dev variant. Everything together is <20MB — the "98% native" rule:
   image stays a ~1.2GB download, no 3GB bloat.
3. **Cursor visible under SDL**: the builder writes `~/.config/hypr/monitors.lua`
   with `cursor = { invisible = true }` (a VNC-era assumption). Must be `false`
   for SDL. (Config is Hyprland Lua — edit the .lua, not hyprland.conf.)
4. **SDDM autologin written by provisioning**: after the setup form creates the
   user, provisioning must write `/etc/sddm.conf.d/autologin.conf` with
   `[Autologin]\nUser=<user>\nSession=hyprland-uwsm`. Proven config. Rule from
   hands-on feedback: Omarchy-branded screens are fine; the generic SDDM greeter
   must never appear.
5. **Bake the clipboard bridge**: install `scripts/guest/clipboard-bridge.sh`
   (this repo) as an executable on PATH, plus
   `scripts/guest/clipboard-bridge.service` as a systemd **user** unit enabled
   for provisioned users (WantedBy=graphical-session.target; uwsm imports
   WAYLAND_DISPLAY so it works). Host counterpart is already in the launcher.
6. **9p automount**: mount tag `hostshare` → `/mnt/host`, systemd.mount with
   `nofail` + `x-systemd.device-timeout` so boots without `-Share` are unaffected
   (`mount -t 9p -o trans=virtio hostshare /mnt/host` is the manual form).
7. Stretch: wire up plymouth (it ships in the image) or otherwise hide boot
   console text on tty1.

Then: `zstd` the rootfs, publish vmlinuz/initramfs/build-spec/rootfs.ext4.zst as a
**v0.0.2-preview** GitHub release on tsouth89/try-omarchy-windows, bump `$release`
in `scripts/bootstrap.ps1`, and validate on the laptop with `-Fresh` (the laptop's
current VM has all of the above hand-installed — a stale disk will mask image bugs).

## Gotchas for guest-image work (details in docs/FINDINGS.md)

- Writes shortly before guest poweroff can be lost under stock WHPX (the poweroff
  wedge) — `sync` in any provisioning step that writes late.
- QMP screendump doesn't work on the GL path — verify via serial
  (`cmd | sudo tee /dev/ttyS0` lands in the host-side serial log).
- The clipboard bridge protocol is LF-only base64 lines; never put a tr/sed stage
  after socat (pipe buffering stalls small payloads).
- Keep .ps1 files pure ASCII (PS 5.1 reads em-dashes as CP1252 curly quotes).

## State of the world (2026-08-28, all pushed through 278b363)

- Windows side COMPLETE and verified on hardware (Ryzen 5 5625U laptop):
  supervised launcher (`scripts/launch-omarchy.ps1`: GPU auto-detect via WINQ-EMU
  at C:\WINQ-EMU, llvmpipe fallback, launch-wedge watchdog, poweroff-wedge reap,
  reboot auto-relaunch, windowless w-binary, no consoles), focus-scoped Win key +
  "Try Omarchy" branding (winkey-forwarder), **two-way clipboard sharing**
  (clipboard-bridge, survives reboots), **folder sharing** (`-Share`, 9p, GPU
  mode), non-admin bootstrap, SDDM autologin (hand-applied in the laptop VM).
- Feature scorecard vs try-omarchy macOS v0.2.0 (Eduardo, 2026-08-28): folder
  sharing ✓, clipboard ✓ (ours is Wayland-native), screensavers ✓, package
  installs ✓ (full x86_64 vs ARM subset), Vulkan ✓ (his is GL-only). Behind only
  on Omarchy 4.0.1 → fixed by this rebuild.
- Repo: https://github.com/tsouth89/try-omarchy-windows • release v0.0.1-preview
  carries the current (old) image artifacts. NOTES.md has the running log;
  docs/FINDINGS.md every trap. Laptop quick start lives in README ("Try it").
