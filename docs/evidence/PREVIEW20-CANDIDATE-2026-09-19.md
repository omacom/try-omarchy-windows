# Preview 20 candidate, September 19, 2026

The compatibility-28 image below is superseded: fresh Windows testing found
that direct drops into the Windows shared mount failed while preserving timestamps.
PR #136 adds a capability check before consuming the transfer ticket. Unsupported
folders use Downloads with a notification; compatibility 29 needs a new build.
All 157 guest tests pass. On the fresh laptop installation, the corrected helpers
passed shared-mount fallback and normal folder delivery, including duplicate names.

Compatibility-28 guest and unsigned launcher source: `e402965018ca3bb8c9563d248354b9381846b935`,
[PR #135](https://github.com/omacom/try-omarchy-windows/pull/135).
Guest compatibility: **28**. Public Latest remains v0.0.19-preview.
This candidate supersedes the compatibility-27 assets with the failed drop UI.

## Changes

- Camera: corrected Media Foundation calls, callback/sample lifetime and capture
  cleanup. Taskbar setup no longer leaks an STA COM apartment onto a pooled thread.
- Audio: SDL supplies microphone capture and playback, with startup fallback to
  DirectSound playback and then no audio.
- File drops: receive verified files in an open local Files folder without a
  transfer window, clipboard change or focus change. Unknown destinations use
  Downloads with a notification. Duplicate names preserve existing files.
- Kernel modules: the external initramfs delivers complete matching TUN and
  camera modules to persistent disks. Incomplete delivery cannot report readiness.
- Package lock refreshed against the build, including Mesa 26.2.3. An interrupted
  package update's orphaned lock is removed only when no live owner can exist.

## Verified

- [Full candidate CI](https://github.com/omacom/try-omarchy-windows/actions/runs/35466919826)
  passed Linux/Windows tests, guest contract checks, the complete guest build and
  a fresh KVM boot. All 153 guest tests passed locally with native GTK enabled.
- The exact rebuilt guest passed a five-boot sequence: v19 seed, upgrade,
  candidate reboot, old-image reboot and return to candidate. File/configuration
  hashes, package ownership and the package database passed throughout.
- The upgraded disk loaded TUN and v4l2loopback before package updates. Module
  metadata and completion markers matched. A deliberately owned pacman lock
  blocked the transaction without being removed.
- All guest and runtime hashes passed. The compressed rootfs was also decoded
  as a stream and matched its raw-image hash. Release asset sizes passed.
- On the Windows 11 Ryzen 5 5625U laptop, the standalone camera passed three
  capture/stop cycles. The user confirmed the physical indicator turned off.
- Full-launcher tests reproduced and fixed the taskbar COM leak. Camera capture
  passed repeated cycles and a clean-launcher restart. A browser captured real
  1280x720 video and stereo 48 kHz microphone input; the user confirmed both.
- The corrected drop helper passed three native Windows drops with matching
  hashes, including duplicate names and a different destination after restart.
  The user repeated the drag and confirmed the behavior was correct.
  See [drop scope and evidence](FILE-DROP-2026-09-19.md).
- Native Windows progress tests passed cancellation and dismissal without
  cancelling the copy.
- A full Windows backup and restore preserved all nine manifest files, including
  the factory image and writable disk. The restored copy booted with WHPX and
  accelerated graphics. The original installation and backup remain intact.
- All 52 native Windows storage/update regression tests passed, including
  failure injection and overwrite protection. These exercise the core operations,
  not every storage dialog on a full guest.
- The rebuilt guest was interrupted before userspace readiness on the restored
  Windows installation. The next launch restored the old payload receipt,
  reached the accelerated desktop without downloading the failed payload again,
  and preserved the user fixture hash. The next ordinary launch successfully
  installed compatibility 28 and loaded TUN and v4l2loopback before any package
  update. Its drop helper matched the reviewed source hash. A second boot
  preserved both compatibility-marker timestamps and the fixture hash, without
  repeating repair.
- PR #135 merged as `58f5b70`. The protected signing check passed, and Windows
  verified the downloaded launcher's Authenticode signature. This diagnostic
  build still identifies as v19; final v20 signed acceptance remains pending.

The physical camera/drop checks used the running test installation, with corrected
scripts installed before the compatibility-28 image was built. Rebuilt-package
and signed-launcher acceptance remain distinct checks. KVM upgrade tests do not
establish Windows update rollback or Windows storage acceptance.

## Candidate identity

| Artifact | SHA256 |
| --- | --- |
| Unsigned launcher | `b0de73a2f0b79419e743957b677dc660f2859783b44439d08d23f28c0e857b83` |
| SHA256SUMS | `7cb0a5f95e9c89af8acad87b5f724375416b7ea3b640a3b6e76cbe9b01861f68` |
| Kernel | `a4671ce8cd648aaf5af83f9a65163c4d6c7083dd63099f16426bd1bd4f0c888f` |
| Initramfs | `f800d9a2a0cac31d085125236f565e35100b76d05dd7c6c0614aeeae6cbf5240` |
| Compressed rootfs | `6874f9b8ed909c79a6590461d64a3ec2b85ec434485651d9d817844b7ed24351` |
| Raw rootfs | `a618db322df39c82c6887923a241b279846f71a2b356d65a8bc428b20435189a` |
| Runtime ZIP | `8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc` |

The unsigned launcher defaults to the public v19 payload. Use the explicit local
asset arguments in [LAPTOP-PASS.md](../LAPTOP-PASS.md) for this candidate.

## Remaining before release

1. Build and authenticate compatibility 29, then verify its packaged delivery
   and fresh boot. Compatibility 28 passed fresh desktop, camera and Vulkan
   playback checks with the signed diagnostic launcher, but failed shared-mount
   drops before the helper correction.
2. Audio playback/device switching, sleep/resume and final physical input/display
   checks on the selected package.
3. Pin the accepted payload, build and verify the signed
   launcher through the protected release workflow, and repeat signed acceptance.
4. Publish only after those checks pass. The user requested release once verified.

This is a preview milestone. Broader Windows/GPU coverage, portable lifecycle,
native migration and stable-version update paths remain v1 gates.
