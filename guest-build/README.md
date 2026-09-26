# Guest image build

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s
`win` branch guest builder, plus the patches in this directory. The release
helper checks out the exact commit in `source.lock.json`, applies every patch,
and runs the guest contract tests before building:

```bash
scripts/release/build-guest.sh --contract-only
scripts/release/build-guest.sh --output /path/to/artifacts
```

Patch 0091 adds a Windows audio endpoint mirror to the guest PipeWire picker.
The bridge talks over a dedicated virtio serial port, and compatibility
revision 33 delivers its user service to existing persistent disks. It needs
the host catalog and live SDL route support in the matching Windows launcher
and runtime; older launchers leave the service waiting for its port.

Patch 0093 brings the pinch touchpad's device-only rules to existing guests.
Compatibility revision 34 delivers the rules file, and catch-up appends a
guarded loader to each user's `input.lua` once, keeping the original as
`input.lua.before-try-omarchy-pinch`. libinput ignores the pinch device until
`try-omarchy-pinch-ready` confirms at boot that every desktop user's config
loads the rules; `/run/try-omarchy/pinch-gestures` says why when it does not.

Patch 0095 lists `virtio-pinch-pci` under `runtime.optionalDevices` in the
build spec. The Windows launcher attaches the pinch touchpad by default only to
guest images that declare it, so older images never receive the device.

Patch 0096 keeps the display mode that `omarchy-native-display-sync` applied
from the window's EDID across `hyprctl reload`, which every theme switch runs.
The script records the mode under `$XDG_RUNTIME_DIR/try-omarchy/display-mode`
and the QEMU profile in `monitors.lua` applies it on each config load, so the
reload no longer modesets back to "preferred" and blanks the window. A
customized `monitors.lua` keeps the older profile, so the script also applies
the EDID mode again on `configreloaded`; there the window keeps its size but
can still blank briefly. Compatibility revision 35 delivers the script and
fragment to existing guests.

The second command needs Docker and currently takes about ten minutes. Release
CI also boots the resulting factory image with `scripts/release/smoke-guest.py`
before it uploads anything.

What the patches change (the original graphics path was proven on hardware
2026-08-28; later additions are covered by contract, release-smoke, and nested
Windows VM tests unless noted in the release checklist):

- Compatibility revision 22 carries the packaged Neovim theme link and its
  catch-up repair to existing guests. Revision 21 delivered the corrected
  runtime repository to existing guests even when the external kernel is
  unchanged. Revision 20 carries Venus presentation workarounds into both
  the UWSM desktop and login shells, including persistent-disk upgrades.
  `VN_PERF=no_async_present` avoids the Mesa 26.2.2 acquisition/presentation lock
  deadlock reproduced on the Windows AMD renderer. The supported loader option
  `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` keeps driver code available for
  thread-exit callbacks after instance destruction. Mappings remain until process
  exit; Vulkan device resources are still explicitly destroyed. Additional Venus
  flags and explicit loader overrides are preserved. Remove these workarounds
  only after default Vulkan playback and thread teardown pass with an upstream
  correction. See the Windows laptop acceptance report for the failure traces.

- Omarchy pin bumped to the v4.0.3 release tag, staged runtime stamped 4.0.3
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
  unit enabled for all users; carries text and PNG images both ways (host side
  is the launcher's clipboard bridge)
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

Patch 0047 supplies the upstream lock PAM profile in fresh images and repairs
only missing profiles on older guests. Existing administrator policies remain
intact, including during runtime package upgrades.

Existing guests can install the image's Omarchy runtime through the normal
**Update > Omarchy** action. See [guest upgrades](../docs/GUEST-UPGRADES.md) for
the delivery mechanism, recovery, and validation requirements.

If Arch has moved since the lock was written, refresh it first and review the diff.
`scripts/release/refresh-guest-lock.sh` does the whole dance: it checks out the
locked source, applies the patches, resolves the lock in Docker, and writes the
next numbered `Refresh-the-guest-package-lock` patch here when anything changed.
The `Refresh guest lock` workflow runs it every Monday and opens a draft pull request
with the package changes. If GitHub policy blocks bot PRs, its run summary links
to the generated branch for manual review; `--check` reports drift without writing a patch.

```bash
scripts/release/refresh-guest-lock.sh
```

Patch 0066 limits runtime command ownership to materialized upstream commands,
bumps the runtime package to `4.0.3-4`, and preserves the two dependency-owned
Neovim helpers when upgrading older runtime packages. Database consistency is
checked during registration and guest smoke testing. See the
[runtime ownership validation](../docs/evidence/RUNTIME-OWNERSHIP-2026-09-14.md).

Patch 0069 retains the `omarchy-nvim` package skeleton when materialization
replaces `/etc/skel/.config`. The package seeds `/etc/skel/.config/nvim` and a
separate `/usr/share/omarchy-nvim/config` copy that omits
`lua/plugins/theme.lua`, which is a relative symlink to the active theme's
generated `neovim.lua`. Rebuilding the skeleton from the package directory alone
dropped that symlink, so new accounts opened Neovim without the Omarchy
colorscheme and `pacman -Qk omarchy-nvim` warned about a missing file. The seed
is now stashed across the replacement. Compatibility revision 22 adds the link
to the compat overlay for existing disks and has `catch-up` recreate it for
users who have a packaged Neovim config but no theme link, without replacing a
file they wrote themselves.
