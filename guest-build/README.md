# Guest image build

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s
`win` branch guest builder, plus the patches in this directory. The release
helper checks out the exact commit in `source.lock.json`, applies every patch,
and runs the guest contract tests before building:

```bash
scripts/release/build-guest.sh --contract-only
scripts/release/build-guest.sh --output /path/to/artifacts
```

Patches 0121 and 0122 port the everyday guest fixes from Try Omarchy for Mac:
screensaver text fits the terminal and tracks its effect PID; low disk space
messages distinguish the guest disk from the PC and point to **Settings >
General > Storage > Disk capacity (GiB)**; first-run update notifications use
normal priority; power profiles show **Managed by Windows** without calling
Linux power-profile tools. Chromium receives `--enable-wayland-ime`, new fcitx5
profiles offer Chewing after the US keyboard, and Traditional Chinese text
prefers Noto CJK TC fonts.

Revision 46 retains the audio transport delivery from #278.
Compatibility revision 47 delivers reviewed payloads and applies them only to
matching default files. It keeps custom content, missing commands and symlinked
configs, seeds only missing font/input profiles, and runs once for disks that
have not reached revision 47.
Patch 0123 locks Chewing and refreshes the resolved package transaction.
Chromium flags in existing homes change only when identical to the upstream
seed. Chewing and man-db are runtime dependencies, so existing disks receive
missing packages with their next **Update > Omarchy**. Lock PAM seeding,
passwordless theme/DNS actions and man-db in factory images were already covered.
The power display is informational; the current bridge opens Try Omarchy
Settings and has no Windows power-settings action.

Patch 0124 exposes the Windows battery's design and full-charge energy, cycle
count, manufacturer, model and chemistry where its driver reports them. Energy
uses microwatt-hours, matching Linux `energy_full_design` and `energy_full`.
The state message is unchanged; older guests ignore the separate details message,
and older launchers leave the new properties unavailable. Windows details are
queried hourly, and failures or unknown values do not invent readings. Charge
limits are not mirrored.
Compatibility revision 48 delivers the bridge and battery DKMS source 1.1.0 to
existing disks. Catch-up retires 1.0.0 while preserving the launcher-delivered
module, registers the new source, and rebuilds when matching headers exist.
The force-install entry from patch 0118 remains in place for later header updates.

Patch 0130 restores notification popups using a read-only lock-state boolean
from the shell's private authentication store. Lock services remain private;
popups and their timers stay hidden and paused during startup, lock and
screensaver activity. Compatibility revision 52 delivers the matching shell
files to existing disks, preserving custom and linked files. Runtime `4.0.4-5`
includes the fix for the next **Update > Omarchy**.

Patch 0132 pauses Hyprland config autoreload while pacman replaces Omarchy
files, preventing a temporary missing-bootstrap error banner during live updates.
Compatibility revision 54 delivers the hooks and transaction guardian to existing
disks before login. The guardian restores each session's previous setting after
one explicit reload, including failed or interrupted transactions. It does not
change user config files or enable a service on the next boot. Hook failures
print a diagnostic and let the package update continue.

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

Patch 0098 stages an opt-in Windows Hello sudo broker and a root-only virtio
authentication port. Compatibility revision 36 delivers the broker and udev
rule to existing persistent disks. No PAM rule is installed until the owner
enables it after guest password authentication. It requires the matching host
approval bridge; without that bridge, password authentication remains available.

Patch 0099 adds an opt-in 1Password unlock that reuses the Windows Hello
pairing: a root-only polkit agent for the installed 1Password app asks the
broker for a signed `onepassword-unlock` approval, and every other request
goes to the standard password dialog. Compatibility revision 37 delivers the
agent, dialog and unit to existing guests without enabling anything.

Patch 0100 drags dropped files into the app under the pointer: after the
files reach Downloads, a small overlay drag source asks the launcher for
`drop-drag`, and the launcher drags from it to the drop point through the
virtual tablet. Compatibility revision 38 delivers the helper to existing
guests.

Patch 0101 makes `try-omarchy-runtime` depend on the integration packages in
the spec's `integrationDepends` and raises its package release to 5, so the
next Update > Omarchy installs any that a disk created by an older image lacks.

Patch 0102 applies fixes from an independent review of the Windows Hello
broker, the 1Password agent, drop delivery and display sync (compatibility
revision 39).

Patch 0103 keeps a monitor scale that the user set in `monitors.lua` across
display sync and Hyprland config reloads. The script reads
`omarchy_monitor_scale`; a number wins over the scale guessed from the EDID,
and "auto" keeps the old behavior. The mode follows the window size, so the
scale is rounded to the closest value Hyprland accepts for that mode.
Compatibility revision 40 delivers the script and the fragment to existing
guests.

Patch 0104 turns off system service watchdogs with a top-level `service.d`
drop-in. The guest clock keeps running while Windows sleeps, so after a resume
systemd treated logind, journald and udevd as hung and restarted them under the
running desktop. Compatibility revision 41 delivers the drop-in to existing
guests.

Patch 0107 makes SSH accept only keys for the quick-start account, whose
password (`omarchy`) is public and whose sudo asks for no password. A forward
to guest port 22 can be bound to the LAN, and Arch's default sshd config allows
password login. A `Match User omarchy` drop-in in `/etc/ssh/sshd_config.d`
turns that off for this account only. New quick-start installs get it when the
account is created, and catch-up adds it once to existing quick-start disks.
The first-desktop notice now says "Quick-start login" and suggests `passwd`.
Compatibility revision 42 delivers the drop-in and scripts to existing guests.

Patch 0112 moves `try-omarchy-export` onto the Try Omarchy importer in
[`migrate/`](../migrate/README.md). The release helper builds the importer into
`/usr/local/lib/try-omarchy/try-omarchy-import.pyz` after applying the patches,
so the guest, the release asset and every export carry the same code. The
command is now a wrapper that runs `try-omarchy-import.pyz export`. The archive
holds only what the user changed in the groups they pick, the skeleton copies
of those files as merge bases, and the importer with an `import.sh` that runs
it on the new install. Keys, sign-ins and browser profiles are only included
when picked. Compatibility revision 44 delivers the wrapper and the importer to
existing guests.

Patch 0114 updates the guest to Omarchy 4.0.4. That release moves bare-metal
installs to Omarchy's own kernel through a migration that installs
`linux-omarchy` and adds it to the Limine boot menu. The guest has no bootloader
and boots the kernel the launcher supplies, so the build replaces that one
migration with a step that only prints a message. The rest of the release is
installer and hardware files the guest does not use. The runtime package becomes
`4.0.4-1`, so Update > Omarchy brings existing guests to it.

Patch 0116 stops the launcher's initramfs from copying `vdso/` into
`/usr/lib/modules/<version>` on the disk. `linux-headers` owns that directory,
and an unowned copy made pacman refuse the next `linux-headers` upgrade, which
failed the whole Update > Omarchy after a kernel bump. The archive already left
out `build/` for the same reason.

Patch 0117 keeps catch-up from putting the quick-start SSH rule back after the
user removed it. Catch-up records adding the rule to an older disk, but disks
created since revision 42 get the rule with the account and had no record, so
the next revision would have added it again. Catch-up now skips the step on any
disk that already ran it at revision 42 or later.

Patch 0118 stops the first Update > Omarchy after a kernel change from printing
"Installation aborted" twice. The launcher delivers its kernel's camera and
battery modules, and when matching headers arrived DKMS refused to install an
identical module. A `modules_to_force_install` entry for just those two modules
lets DKMS replace them, keeping the delivered copy and putting it back when the
headers are removed. Compatibility revision 45 delivers it to existing guests.

Patch 0119 stops Update > Omarchy from asking to reboot for a new kernel on
disks older than their launcher. Omarchy asks whenever no installed kernel
package matches the running kernel, but here the launcher supplies the kernel
and the guest's `linux` package stays held, so rebooting changed nothing. The
build turns that one check off and fails if Omarchy moves it. The runtime
package becomes `4.0.4-2` so existing guests get it.

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
- The quick-start account (`omarchy`, whose password is public) accepts only
  SSH keys through a `Match User omarchy` drop-in; own accounts keep password
  login
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

Patch 0120 removes hidden playback attenuation behind the Windows route picker.
The bridge keeps the virtio ALSA transport at 100% while its remap sinks are
active; the visible route controls guest volume. It preserves mute and restores
the previous transport channel volumes on orderly shutdown unless the owner
changed them. Compatibility revision 46 delivers the bridge to existing disks.
See [audio behavior](../docs/AUDIO-DEVICES.md) for the signal path and checks.

Patch 0126 allows Media Player to use Mesa software rendering when the launcher
boots with CPU rendering. GPU mode retains mpv defaults, and explicit command-line
options take precedence. Compatibility revision 49 delivers the wrapper to existing
guest disks.

Patch 0128 synchronizes the visible output volume and mute with the Windows
default playback endpoint through the existing audio port. While enabled, raw
null-sink monitors feed the unity-gain virtio transport so only Windows applies
master gain and mute. The original remap graph returns when sync is disabled,
the bridge disconnects or host volume state is absent for ten seconds. The current
guest volume and mute survive fallback; a capable host can enable sync again.
Compatibility revision 50 delivers the updated bridge to existing disks.

Patch 0129 ports the manual night-light shader from [Mac PR #251](https://github.com/omacom/try-omarchy/pull/251). The existing night-light menu and Super + Ctrl + N use the same serialized backend on `omarchy.qemu=1`, since virtio GPU lacks DRM CTM. It refuses to replace custom screen shaders and reads status from Hyprland. A config reload clears the manual tint and refreshes the indicator. Compatibility revision 51 updates only known command and service defaults on existing disks, preserving customized or linked files. Runtime package 4.0.4-4 owns the command, helper and shader.

Patch 0131 reports compositor IPC health over the guest agent channel every five
seconds. Each probe gives Hyprland two seconds to answer. Compatibility revision
53 delivers the agent and helper to existing disks on their next boot. Login and
logout report an inactive compositor. Launchers without health monitoring ignore
these messages; guests without heartbeats do not trigger compositor warnings.
IPC health does not certify that a frame reached the physical display.

Patch 0133 lets Update > Omarchy adopt only the two unowned night-light backend
files delivered by compatibility catch-up. It uses the updater's existing
move, retry and restoration path, retaining previous bytes for recovery and
stopping on unrelated or package-owned conflicts. Compatibility revision 55
repairs only the reviewed default conflict handler before login, including disks
that already reached revisions 51 through 54. Runtime `4.0.4-6` packages the same
handler for fresh guests.

Patch 0135 restores the one-second retry of the Windows audio catalog while
PipeWire's QEMU transport is not ready at startup. It keeps the catalog until
the routes exist, then continues the volume handshake. Compatibility revision
56 delivers the corrected bridge to existing disks, including v0.3.0 through
v0.9.0 guests and disks already at revision 55. Runtime `4.0.4-7` carries the
matching package release for fresh installations and subsequent updates.
