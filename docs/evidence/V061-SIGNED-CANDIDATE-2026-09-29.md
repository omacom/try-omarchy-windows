# v0.6.1 signed candidate and release acceptance - September 29, 2026

v0.6.1 adds a setting that gives Alt+Tab back to Windows (#217), stops guest
service watchdogs from killing logind, journald and udevd after Windows wakes
from Modern Standby (#218), keeps a monitor scale set in `monitors.lua` across
config reloads (#214), and refreshes the guest package lock (#219, #221). The
guest image carries patches through `0106` (compatibility revision 41) and
`try-omarchy-runtime` package release 5. The runtime is unchanged: r20c.

## Candidate

- The first prepare run failed on lock drift: Arch published librsvg 2.62.4
  about two hours after the #219 refresh. A new refresh (#221) landed and
  [prepare run 36574864437](https://github.com/omacom/try-omarchy-windows/actions/runs/36574864437)
  built the draft from `4863b31`, including the headless first-boot smoke test.
  `SHA256SUMS` digest
  `ce9c1748c801633e78b39b94489f419164c1189f49bc8727014aaf6fcd958d90`.
- Pin pushed to master as `523f625`. The
  [signing-check run 36576032145](https://github.com/omacom/try-omarchy-windows/actions/runs/36576032145)
  produced the tested launcher, SHA-256
  `9161763055b7a3aa2acd68bb84f6e62e1fa8d2603420fb243c8c2507ef9ecd36`.
  On the laptop, Authenticode was `Valid`, signer `CN=Brandon South`, and
  FileVersion and ProductVersion were `v0.6.1`. Every asset copied to the
  laptop matched `SHA256SUMS`.
- Assets were served over loopback on the laptop from `127.0.0.1:18080`, and
  every candidate launch pinned both `-release` and `-runtime-release` to it.

All runs were on the AMD Windows 11 laptop under WHPX with GPU rendering. The
owner's real installation was not started or changed.

## Rollback, then upgrade from v0.6.0 (copy 2)

Copy 2 was the v0.6.0 install left by the v0.6.0 acceptance run
(compatibility revision 39).

- The candidate kept the unchanged runtime archive, installed the new guest,
  and a watcher stopped the launcher and QEMU in the same second the first
  v0.6.1 boot started. `payload-update-state.json` recorded the guest pending
  with `guest.previous` retained.
- The next launch logged `using restored guest and runtime for this recovery
  launch`, requested nothing from the payload server and booted v0.6.0:
  compatibility revision 39, `running`, 0 failed units, marker
  `~/Documents/v010-clean-marker.txt` unchanged (`d7c3d98d...`).
- After a clean poweroff, the next launch downloaded the payload again and
  logged `guest update v0.6.1 confirmed after userspace reported ready`.
  Compatibility revision 41, `running`, 0 failed units, marker unchanged, no
  Hyprland config errors. The watchdog drop-in was installed and logind,
  journald, udevd, userdbd and sshd all reported `WatchdogUSec=0`.
- Monitor scale (#214): with `omarchy_monitor_scale = 2`, the display used
  scale 2 at 1184x662 and kept it across two `hyprctl reload`s. At sizes where
  2 does not divide the mode (1366x697, 1370x749, 1185x662, 1184x661) it used
  1, with no config errors.
- Alt+Tab setting (#217), changed through the Settings dialog while Omarchy
  ran:
  - Saving without touching the checkbox did not create
    `keyboard-preferences.json`.
  - On (the default), Alt+Tab moved guest focus and Omarchy stayed in front.
  - Off logged `keyboard: Alt+Tab goes to Windows` one second after Save. Held
    Alt+Tab showed the Windows Task Switching overlay, and release focused
    File Explorer. Guest focus did not move.
  - Back on logged `keyboard: Alt+Tab goes to Omarchy while it is focused` and
    Alt+Tab moved guest focus again.
  - The signed v0.6.0 launcher opened Settings normally with the new file
    present.
- Modern Standby (#218): the launcher and QEMU were suspended together with
  `NtSuspendProcess` for 5 minutes. Afterwards Hyprland, the shell, logind,
  journald and udevd kept their PIDs, the seat session and both open windows
  remained, there were 0 failed units and no watchdog or timeout lines in the
  system or user journal, and Alt+Tab moved guest focus.
- An in-guest reboot relaunched through the supervisor to `running` with 0
  failed units. A launch after a clean poweroff requested only `SHA256SUMS`.

## Fresh install

A new data directory with `-instant` downloaded the payload and reached
userspace. The test answered "Not now" to the shared folder and cleared the
shortcut choices; no Start menu or Desktop shortcut was created.

- Compatibility revision 41, systemd `262-1`, hyprland `0.56.2-3`, linux
  `7.2.7.arch1-1`, `try-omarchy-runtime 4.0.3-5`, librsvg `2.62.4-1`, glibc
  `2.44+r50`; `running` with 0 failed units; 24 GiB root filesystem.
- Watchdog drop-in active on the same five services. Pinch ready with its
  loader in `input.lua`, background and bar mapped, three PipeWire sinks, drop
  helper and Hello broker present, no Hyprland config errors once IPC answered.
- The Alt+Tab save, on, off and on-again checks above passed again.
- The same 5-minute freeze, started when the desktop session was four minutes
  old, restarted nothing: same PIDs and session, both windows, 0 failed units,
  no watchdog or timeout lines. Alt+Tab worked afterwards.
- A guest reboot relaunched with pinch forwarding on and userspace ready.

## Publication and public update

[Publish run 36608784130](https://github.com/omacom/try-omarchy-windows/actions/runs/36608784130)
signed, published and verified the release, then marked v0.6.1 Latest. The
public launcher SHA-256 is
`02c12abdbeafbff2b71f5f126dc3fd21b0b6916533f5932420845402aef7fcb9`; it is a
separate signed build of the same commit as the tested candidate.
`releases/latest/download/TryOmarchy.exe` and `tryomarchy.com/download`
resolve to it, and the master CI pin check passed after publication.

Copy 1, the install the v0.6.0 public update test left on v0.6.0, was started
through its own installed v0.6.0 launcher (SHA-256 `631ae4dd...`) with no
release, runtime or update overrides.

- The first attempt found the signed feed, logged `starting authenticated
  launcher update` and restarted as v0.6.1 within a second. The guest download
  then stopped at its space preflight: the test drive had 7.0 GiB free and the
  new guest needed 7.4 GiB. The launcher showed "Your PC is out of disk space.
  Free up a few gigabytes, then start Try Omarchy again." and wrote nothing.
- With the launcher update unconfirmed, the next start restored the v0.6.0
  launcher and booted the v0.6.0 guest (compatibility revision 39, `running`).
- After freeing space, powering off, and clearing the 4-hour update check
  throttle on this copy, the next launch updated the launcher again, downloaded
  the v0.6.1 guest from GitHub and logged `launcher update v0.6.1 confirmed
  after healthy boot` and `guest update v0.6.1 confirmed after userspace
  reported ready`.
- The installed launcher became v0.6.1 with the public SHA-256 above. The guest
  ran compatibility revision 41 with 0 failed units, the watchdog drop-in
  active, pinch ready and no Hyprland config errors. It powered off cleanly.

## Not covered

A real Modern Standby laptop (this one only has S3, where the launcher pauses
the guest before sleep), a real S3 sleep on this exact build, Windows 10, other
GPUs and touchpads, and running Update > Omarchy on the laptop. The launcher
still does not pause the guest on Modern Standby; this release only stops the
watchdog kills.
