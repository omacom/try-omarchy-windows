# Preview 20 candidate, September 19, 2026

Guest source: `0f8f371afc9676f6351404ae8b9e5b364e1bf9cc`.
Launcher source: `11227bda63659418944ebb4561ed0027d1438b08`, [PR #135](https://github.com/omacom/try-omarchy-windows/pull/135).
Guest compatibility: **27**. This is a diagnostic candidate, not release approval.
Public Latest remains v0.0.19-preview.

The physical file-drop test failed with these guest assets. The subsequent
[background drop correction](FILE-DROP-2026-09-19.md) is installed and tested on
the laptop, but these compatibility-27 artifacts must not be published as the
corrected candidate. A compatibility-28 guest rebuild is in progress.

## Changes

- Camera: corrected COM interface IDs and method slots, sample ownership,
  callback lifetime, COM thread ownership, format conversion and capture cleanup.
  The guest service now starts in the graphical session with working device access.
- Audio: SDL supplies microphone capture and playback. Startup failure can fall
  back to DirectSound playback, then no audio. Physical browser microphone capture passed on the test laptop.
- File drops: use QMP routing, scale drop coordinates to the guest display, check
  command failures, and keep received files visible even after a paste request.
  Shortcut injection does not establish application acceptance.
- Kernel modules: the external initramfs supplies a complete matching module tree,
  including TUN and v4l2loopback, to persistent disks. Incomplete delivery cannot
  report readiness. The factory disk no longer stores a duplicate external initramfs.
- Refreshed the package lock against the successful build transaction, including
  Mesa 26.2.3. Graphics acceptance must use these exact assets.

## Verified

- [Linux and Windows CI](https://github.com/omacom/try-omarchy-windows/actions/runs/35460222193)
  passed. Windows tests exercise real Media Foundation sample/buffer APIs and
  callback ownership without requiring a physical camera.
- All **150 guest tests** passed locally with the native GTK test enabled.
- [Full guest build and fresh-boot smoke](https://github.com/omacom/try-omarchy-windows/actions/runs/35460228239)
  passed, including loading TUN and v4l2loopback and checking the camera service link.
- A verified v19 image was copied into a disposable persistent disk. The existing
  upgrade harness passed seed, candidate upgrade, candidate reboot, old-image
  reboot, and return-to-candidate phases. File/configuration hashes, package
  ownership and the package database passed throughout.
- Before the package update, the upgraded disk loaded TUN and v4l2loopback,
  exposed /dev/net/tun and /dev/video42, and had complete module metadata.
  A deliberately owned pacman lock blocked the transaction without being removed.
- Release assets passed hash and per-file size checks.
- On the Windows 11 Ryzen 5 5625U laptop, the standalone camera passed three
  capture/stop cycles. The user confirmed the physical indicator turned off.
- Full-launcher testing exposed leaked STA COM initialization in taskbar setup.
  Balancing initialization on a locked thread fixed the camera MTA failure.
  The native regression test reproduced the failure before the fix and passed after.
- The clean `11227bd` launcher captured 60 distinct frames after a restart.
  The preceding diagnostic build of the same source passed three 45-frame cycles.
- A real-device Chromium test captured 193 video frames at 1280x720 and stereo
  microphone input at 48 kHz. The user confirmed video and microphone operation.
  Capture was stopped afterward. No camera or microphone recordings were saved.
- Concurrent backup activity on the USB SD card caused severe guest I/O wait.
  Pausing the backup cleared the delay; storage tests now run separately from
  interactive guest tests.

The five-boot test used Linux KVM. It does not establish signed Windows launcher
update/rollback, Windows storage behavior, GPU behavior, or the remaining physical audio/device lifecycle checks.

## Candidate identity

| Artifact | SHA256 |
| --- | --- |
| Unsigned launcher | `94ec5e2a1ad57a2d2604eec4688a69d953c5901b54637d9bc9bbce1f288a703c` |
| SHA256SUMS | `99729b81934a42090e490e6c87a728438085c4d203015009f0f055efdb3e8a61` |
| Kernel | `a4671ce8cd648aaf5af83f9a65163c4d6c7083dd63099f16426bd1bd4f0c888f` |
| Initramfs | `092dad8d8c22b1e34813e27c2a75c865379af0d1c0041fe13ea49465e8a97be9` |
| Compressed rootfs | `3da4cd5c90787a4a18ebefc5bcd01f6a90fc8fea90d3ba29b644654523e69807` |
| Runtime ZIP | `8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc` |

The unsigned launcher still defaults to the public v19 payload. Use the explicit
local-asset arguments in [LAPTOP-PASS.md](../LAPTOP-PASS.md) to test this complete
candidate. Double-clicking the launcher alone does not exercise the new guest.

## Remaining before release

1. Windows audio playback, device switching, sleep, drops and broader graphics acceptance.
2. Exact-candidate Windows update failure/rollback and storage operations on copied
   data: move, backup/restore, snapshots, growth/reclaim and low-space recovery.
3. Merge the reviewed fixes, pin the accepted payload, build and verify the signed
   launcher through the protected release workflow, and repeat signed acceptance.
4. Publish only after acceptance and an explicit publication request.

Website corrections are in [a separate draft PR](https://github.com/btsouth/tryomarchy-site/pull/1).
Its hosted preview was checked; the production site is unchanged.
