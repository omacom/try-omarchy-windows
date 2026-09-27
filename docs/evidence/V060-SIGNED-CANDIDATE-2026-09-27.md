# v0.6.0 signed candidate and release acceptance - September 27, 2026

v0.6.0 lets files dropped from File Explorer land in the app under the pointer
(#206), unlocks 1Password with Windows Hello (#202), applies local port-forward
changes while Omarchy runs (#208), brings the integration packages newer images
depend on to existing disks (#207), and tightens the Windows Hello sudo path
(#209). The guest image carries patches through `0102` (compatibility revision
39) and `try-omarchy-runtime` package release 5. The runtime is unchanged:
r20c.

## Candidate

- The first prepare run failed its instant boot smoke test because the test
  still expected runtime package release 4. #211 now reads the expected version
  from `build-spec.json`, and
  [prepare run 36298580348](https://github.com/omacom/try-omarchy-windows/actions/runs/36298580348)
  built the draft. `SHA256SUMS` digest
  `5630740cbaeec8e043f4f1ada8699198e2ea568ff1295538461dac6da925f427`.
- Pin pushed to master as `6b7f38f`. The
  [signing-check run 36299022521](https://github.com/omacom/try-omarchy-windows/actions/runs/36299022521)
  produced the tested launcher, SHA-256
  `0f9d4217aa6fe9e60affe0322cc50ffa98189dc53d87e15b53854530c6cc8b84`.
  On the laptop, Authenticode was `Valid`, signer `CN=Brandon South`, and
  FileVersion and ProductVersion were `v0.6.0`. Every uploaded asset matched
  `SHA256SUMS`.
- Assets were served over loopback on the laptop from `127.0.0.1:18080`, and
  every candidate launch pinned both `-release` and `-runtime-release` to it.

All runs were on the AMD Windows 11 laptop under WHPX with GPU rendering. The
owner's real installation was not started or changed.

## Rollback, then upgrade from v0.5.0 (copy 2)

Copy 2 was the v0.5.0 install left by the v0.5.0 acceptance run.

- The candidate began its first v0.6.0 boot at 01:30:39 and a watcher stopped
  the launcher and QEMU within the same second. `payload-update-state.json` recorded
  the guest pending with `guest.previous` retained; the runtime archive was
  unchanged, so it was kept.
- The next launch requested nothing from the payload server and booted the
  restored v0.5.0 guest: compatibility revision 36, `running`, 0 failed units,
  `try-omarchy-runtime 4.0.3-4`, marker `~/Documents/v010-clean-marker.txt`
  unchanged (`d7c3d98d...`).
- After a clean poweroff, the next launch downloaded the payload again and
  logged `guest update v0.6.0 confirmed after userspace reported ready`.
  Compatibility revision 39, `running`, 0 failed units, marker unchanged, pinch
  ready, no Hyprland config errors, and the drop helper and Hello broker in
  place. Every package in `integrationDepends` was installed. The guest's update
  repository offered `try-omarchy-runtime 4.0.3-5`, which Update > Omarchy
  installs; release CI's upgrade smoke test ran that update from v0.5.0.
- Relaunched with SSH set up from `settings.json` instead of flags, so the live
  forward watcher ran. Saving an extra `18765:18765` forward logged
  `forwards: applied 1 added, 0 removed while running` and Windows fetched the
  guest's `compat-version` through it; removing it logged `0 added, 1 removed`
  and the port closed.
- A TCP connection to the Windows Hello port from PowerShell was closed at once
  and logged `Windows Hello: refused a connection that is not from Omarchy's
  QEMU`.
- An in-guest reboot relaunched through the supervisor with pinch ready and 0
  failed units. A launch after a clean poweroff requested only `SHA256SUMS`.

## Fresh install

A new data directory with `-instant` downloaded the payload and reached
userspace. The test answered "Not now" to the shared folder and cleared the
shortcut choices; no Start menu or Desktop shortcut was created.

- Compatibility revision 39, systemd `262-1`, hyprland `0.56.2-3`, linux
  `7.2.7.arch1-1`, `try-omarchy-runtime 4.0.3-5`; `running` with 0 failed
  units; 24 GiB root filesystem.
- Pinch ready with its loader in the new user's `input.lua`, the display-sync
  profile in `monitors.lua`, no Hyprland config errors, background and bar
  mapped, three PipeWire sinks, drop helper and Hello broker present. Files
  opened from `omarchy-launch-nautilus` and stayed open.
- Two Hyprland queries in the first minute after boot timed out; the same
  queries answered in under 20 ms a minute later.
- A guest reboot relaunched with pinch forwarding on and userspace ready.

## Publication and public update

[Publish run 36301605995](https://github.com/omacom/try-omarchy-windows/actions/runs/36301605995)
signed, published and verified the release, then marked v0.6.0 Latest. The
public launcher SHA-256 is
`631ae4dd02799a8f75926a5955294152baf9b8ba0709f2255cec175dc702f50c`; it is a
separate signed build of the same commit as the tested candidate.
`releases/latest/download/TryOmarchy.exe` and `tryomarchy.com/download`
resolve to it, and the master CI pin check passed after publication.

Copy 1, the install the v0.5.0 public update test left on v0.5.0, was started
through its own installed v0.5.0 launcher (SHA-256 `8af8caa6...`) with no
release, runtime or update overrides:

- The launcher logged `starting authenticated launcher update`, replaced itself
  and restarted within five seconds, and kept the installed r20c runtime.
- It downloaded the v0.6.0 guest from GitHub and logged
  `launcher update v0.6.0 confirmed after healthy boot` and
  `guest update v0.6.0 confirmed after userspace reported ready`.
- The installed launcher became v0.6.0 with the public SHA-256 above. The guest
  ran compatibility revision 39 with 0 failed units, pinch ready and no Hyprland
  config errors. It powered off cleanly.

## Not covered

Windows 10, other GPUs and touchpads, a physical Explorer drag onto an app and
Windows Hello prompts on this exact signed build (both passed on the laptop
with the same code before merge in #206, #202 and #209), and running Update >
Omarchy on the laptop.
