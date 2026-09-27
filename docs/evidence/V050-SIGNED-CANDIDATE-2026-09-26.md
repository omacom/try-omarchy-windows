# v0.5.0 signed candidate and release acceptance - September 26, 2026

v0.5.0 adds opt-in Windows Hello approval for guest sudo (#179), keeps the
display mode across theme switches (#197), follows the Windows light or dark
theme in the title bar (#198), keeps Alt held across Alt+Tab taps (#196),
recovers a finished download when its link stops working (#188), and refreshes
the guest lock (#199). The guest image carries patches through `0098`
(display mode `0096`, lock refresh `0097`, Hello broker `0098`; compatibility
revision 36). The runtime is unchanged: r20c.

## Candidate

- Draft built by [prepare run 36282964712](https://github.com/omacom/try-omarchy-windows/actions/runs/36282964712).
  `SHA256SUMS` digest `91f7b364b8a67279a6e2aa688f7ca94c550b9ec645e89c9216f9037f30db0276`.
- Pin pushed to master as `bf5ff9d`. The [signing-check run 36283388937](https://github.com/omacom/try-omarchy-windows/actions/runs/36283388937)
  produced the tested launcher, SHA-256
  `ea8e0bb268d76dfd4449f8f9e48f0c8efa3ee410dbf16d1f777d87632a7943f6`.
  On the laptop, Authenticode was `Valid`, signer `CN=Brandon South`, and
  FileVersion and ProductVersion were `v0.5.0`. Every uploaded asset matched
  `SHA256SUMS`.
- Assets were served over loopback on the laptop from `127.0.0.1:18080`, and
  every candidate launch pinned both `-release` and `-runtime-release` to it.

All runs were on the AMD Windows 11 laptop under WHPX with GPU rendering. The
owner's real installation was not started or changed.

## Rollback, then upgrade from v0.1.0 (copy 2)

Copy 2 was the clean v0.1.0 install left by the v0.4.0 rollback test.

- The candidate began its first v0.5.0 boot at 19:59:32 and a watcher stopped
  the launcher and QEMU 0.4 seconds later. `payload-update-state.json` recorded
  guest and runtime pending, with `guest.previous` and `runtime.previous`
  retained.
- The next launch requested nothing from the payload server and booted the
  restored v0.1.0 guest: compatibility revision 29, kernel `7.2.6-arch2-1`,
  `running`, marker `~/Documents/v010-clean-marker.txt` unchanged
  (`d7c3d98d...`), pinch forwarding off because that image does not declare
  the device.
- After a clean poweroff, the next launch downloaded the payload again and
  logged `guest update v0.5.0 confirmed after userspace reported ready`.
  Compatibility revision 36, kernel `7.2.7-arch1-1`, `running`, 0 failed units,
  marker unchanged. Catch-up refreshed the QEMU profile in `monitors.lua` and
  appended the pinch loader to `input.lua`; pinch reported ready and
  `hyprctl configerrors` was empty. The Hello broker, command and root-only
  `dev.tryomarchy.authentication` port were present, with no PAM rule enabled.
- With the window restored to 1100x620, a 30 fps screen recording around a
  switch to Catppuccin had 0 dark frames out of 360, and the guest stayed at
  1084x581. The QEMU window had `DWMWA_USE_IMMERSIVE_DARK_MODE` set in the
  owner's dark mode. A synthetic hold-Alt, three-Tab sequence logged one
  forwarded Alt and three Tabs.
- An in-guest reboot relaunched through the supervisor with pinch ready and 0
  failed units; catch-up did not repeat its changes. A second launch after a
  clean poweroff requested only `SHA256SUMS`.

## Fresh install

A new data directory with `-instant` downloaded the payload and reached
userspace. The test answered "Not now" to the shared folder and cleared the
shortcut choices; no Start menu or Desktop shortcut was created.

- Compatibility revision 36, systemd `262-1`, hyprland `0.56.2-3`, linux
  `7.2.7.arch1-1`; `running` with 0 failed units; 24 GiB root filesystem.
- Pinch ready with its loader in the new user's `input.lua`, the display-sync
  profile in `monitors.lua`, no Hyprland config errors, background and bar
  mapped, three PipeWire sinks, Hello broker present.
- A guest reboot relaunched with pinch forwarding on and userspace ready.

## Windows Hello

The Hello flow was accepted on the same laptop earlier the same day with a
guest built from the same patches; see
[the laptop run](HELLO-SUDO-LAPTOP-2026-09-26.md). Pairing, one approval, one
cancel falling back to the password, refusal while another window was in
front, and disable all behaved as designed.

## Publication and public update

[Publish run 36285513281](https://github.com/omacom/try-omarchy-windows/actions/runs/36285513281)
signed, published and verified the release, then marked v0.5.0 Latest. The
public launcher SHA-256 is
`8af8caa6e3cc13537e90be56fd18393a558f857a9b68ecdfd1a5bc306c09f8a2`; it is a
separate signed build of the same commit as the tested candidate.
`releases/latest/download/TryOmarchy.exe` and `tryomarchy.com/download`
resolve to it, and the master CI pin check passed after publication.

The public update path was then exercised for the first time. Copy 1, the
v0.4.0 install from the v0.4.0 acceptance run, was started through its own
installed v0.4.0 launcher (SHA-256 `08bdd956...`) with no release, runtime or
update overrides:

- The launcher fetched the signed update feed, logged
  `starting authenticated launcher update`, replaced itself and restarted
  within five seconds, and kept the installed r20c runtime.
- It downloaded the v0.5.0 guest from GitHub and logged
  `launcher update v0.5.0 confirmed after healthy boot` and
  `guest update v0.5.0 confirmed after userspace reported ready`.
- The installed launcher became v0.5.0 with the public SHA-256 above. The guest
  ran compatibility revision 36 with 0 failed units, the marker unchanged,
  pinch ready and no Hyprland config errors. It powered off cleanly.

## Not covered

Windows 10, other GPUs and touchpads, a physical keyboard on this exact build
(the owner's physical Alt+Tab check passed on #196 earlier the same day), and
Windows Hello on this exact signed build.
