# Guest image build

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s
`win` branch guest builder, plus the patches in this directory. The release
helper checks out the exact commit in `source.lock.json`, applies every patch,
and runs the guest contract tests before building:

```bash
scripts/release/build-guest.sh --contract-only
scripts/release/build-guest.sh --output /path/to/artifacts
```

The second command needs Docker and currently takes about ten minutes. Release
CI also boots the resulting factory image with `scripts/release/smoke-guest.py`
before it uploads anything.

What the patches change (the original graphics path was proven on hardware
2026-08-28; later additions are covered by contract, release-smoke, and nested
Windows VM tests unless noted in the release checklist):

- Omarchy pin bumped to the v4.0.2 release tag, staged runtime stamped 4.0.2
  (upstream's `version` file lags its tags)
- Omarchy repository packages must be signed, as in upstream 4.0.2; the builder
  trusts the vendored Omarchy packaging key by fingerprint
- All 22 upstream themes included (was 6)
- Packages: ttfx + hypridle (screensavers), vulkan-virtio (Venus ICD), plus the
  tools behind Omarchy's bound keys and menu entries (gpu-screen-recorder,
  tensaku, tesseract, zbar, qrencode, wtype, plocate, man-db, xdg-terminal-exec,
  omacalc, omawrite, omacut, herdr, hyprland-preview-share-picker)
- yay from the Omarchy repository plus the base-devel toolchain, so upstream's
  AUR entry points (omarchy-pkg-aur-add and friends) work in the guest
- Omarchy's LazyVim configuration and clang, matching the upstream editor setup
- Reviewed Omarchy fixes for notification dismissal and for keeping notification
  contents hidden while the lock screen or screensaver obscures the session
- A fixed 4096-frame PipeWire quantum for stable playback through QEMU's
  emulated Intel HDA device
- Guest cursor visible under SDL (the hidden-cursor fragment was a VNC-era assumption)
- Autologin stays permanent: a drop-in disarms upstream's one-boot autologin
  cleanup after provisioning (the VM window is the auth boundary here)
- Clipboard bridge baked in: /usr/local/bin/clipboard-bridge + a systemd user
  unit enabled for all users (host side lives in scripts/clipboard-bridge.ps1)
- /mnt/host automounts the launcher's `-share` folder (virtio-9p, condition-guarded
  so boots without a share stay clean)
- An explicit `tryomarchy.instant=1` kernel flag creates and finalizes a local
  trial account, shows its credentials once on the first desktop, and leaves
  boots without the flag on upstream's normal setup form
- `tryomarchy.sshd=1` (set by the launcher when a host port forwards to guest
  port 22) starts sshd for that boot only and authorizes the launcher-supplied
  public key; sshd config and enablement stay untouched
- `try-omarchy-export` archives an allowlist of desktop configuration, the
  theme, and added packages with a restore script for a real Omarchy install
  (docs/MIGRATION.md)
- `tryomarchy.sharename=<base64>` links the `-share` folder into the home
  directory under its own name at login, pins it in the Files sidebar, and
  opens each newly selected share once so users can find it immediately; it
  removes only those managed entries on launches that share nothing
- `tryomarchy.tz=` and `tryomarchy.kb=` (set by the launcher from the Windows
  time zone and default input language) are applied at boot by a sysinit
  service when they change, so the guest clock and Hyprland's keyboard layout
  follow Windows without overriding a choice made inside the guest
- New users start with no Hyprland toggles switched on. The builder used to
  copy every toggle template into the user's toggle state, which turned "no
  gaps" and "single-window aspect ratio" on permanently and overrode the gaps,
  border, and rounding set in looknfeel.lua (#32)
- Overlay scripts have unprivileged behavioral tests under guest/tests
- The initramfs carries those launcher-integration files onto persistent disks
  created by older releases and reports userspace readiness before the Windows
  launcher commits a guest-image update. Its explicit integration revision is
  bumped whenever those files must be reapplied without a kernel version change

If Arch has moved since the lock was written, refresh it first and review the diff:

```bash
sudo bash guest/build-container.sh --refresh-package-lock guest/packages.lock.json
```
