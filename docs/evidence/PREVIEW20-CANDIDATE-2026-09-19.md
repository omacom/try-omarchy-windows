# Preview 20 candidate, September 19, 2026

Guest and unsigned launcher source: `946521b4afeab02a6dcd6e3d8f02acc37553c507`,
[PR #136](https://github.com/omacom/try-omarchy-windows/pull/136).
Guest compatibility: **29**. Public Latest remains v0.0.19-preview.
Compatibility 29 supersedes the earlier v20 images.

## Changes

- Camera: corrected Media Foundation calls, callback/sample lifetime and capture
  cleanup. Taskbar setup no longer leaks an STA COM apartment onto a pooled thread.
- Audio: SDL supplies microphone capture and playback, with startup fallback to
  DirectSound playback and then no audio.
- File drops: receive verified files in an open local Files folder without a
  transfer window, clipboard change or focus change. Unknown destinations use
  Downloads with a notification. Duplicate names preserve existing files.
- Before consuming a drop ticket, check that the destination supports timestamp
  preservation and atomic publication without overwriting. Unsupported folders,
  including the Windows shared mount on the test laptop, use Downloads.
- Kernel modules: the external initramfs delivers complete matching TUN and
  camera modules to persistent disks. Incomplete delivery cannot report readiness.
- Package lock refreshed against the build, including Mesa 26.2.3. An interrupted
  package update's orphaned lock is removed only when no live owner can exist.

## Verified

- [Full candidate CI](https://github.com/omacom/try-omarchy-windows/actions/runs/35469290827)
  passed Linux/Windows tests, guest contract checks, the complete guest build and
  a fresh KVM boot. All 157 guest tests passed locally with native GTK enabled.
- The exact rebuilt guest passed a five-boot sequence: v19 seed, upgrade,
  candidate reboot, old-image reboot and return to candidate. File/configuration
  hashes, package ownership and the package database passed throughout.
- The upgraded disk loaded TUN and v4l2loopback before package updates. Module
  metadata and completion markers matched. An owned pacman lock blocked the
  transaction without being removed.
- Guest and locked runtime hashes passed. The compressed rootfs was decoded as
  a stream and matched its raw-image hash. Release asset sizes passed.
- On the Windows 11 Ryzen 5 5625U laptop, the standalone camera passed three
  capture/stop cycles. The user confirmed the physical indicator turned off.
- Full-launcher tests reproduced and fixed the taskbar COM leak. Camera capture
  passed repeated cycles and a clean-launcher restart. A browser captured real
  1280x720 video and stereo 48 kHz microphone input; the user confirmed both.
- The corrected drop helper passed three native Windows drops with matching
  hashes, including duplicate names and a different destination after restart.
  The user repeated the drag and confirmed the behavior was correct.
  See [drop scope and evidence](FILE-DROP-2026-09-19.md).
- A fresh compatibility-28 installation with the signed diagnostic launcher
  reached the accelerated desktop, captured 30 distinct camera frames and played
  a Vulkan test video. QEMU averaged 1.43% of total host CPU over a 30-second idle
  sample. The user reported that normal use was working well.
- That fresh test found the shared-mount timestamp failure. The compatibility-29
  helper correction passed physical shared-folder fallback and normal-folder
  drops, including duplicate names, with matching hashes and no transfer window.
- A full Windows backup/restore preserved all nine manifest files, including the
  factory image and writable disk. The restored copy booted. All 52 native
  storage/update regression tests passed, including failure injection and
  overwrite protection. These do not cover every storage dialog on a full guest.
- On the restored Windows copy, interrupting the compatibility-28 update before
  userspace readiness left it uncommitted. The next launch restored the old
  payload, reached the accelerated desktop without downloading the failed image,
  and preserved the user fixture. Normal retry installed the update successfully;
  a second boot preserved both compatibility-marker timestamps and the fixture.
- The exact compatibility-29 package upgraded the Windows test guest successfully.
  The installed transfer helpers matched source hashes, both module markers were
  complete, and both dropped files plus the audio fixture retained their hashes.
- A clean compatibility-29 Windows guest, initialized from the verified payload
  cache with a freshly installed bundled runtime, reached the accelerated desktop.
  TUN/camera module checks and exact helper hashes passed. A native drop over the
  Windows shared-folder view arrived in Downloads with the expected hash, without
  a transfer window or leftover staging files.
- PR #135 merged as `58f5b70`. The protected signing check passed; Windows verified
  Authenticode and the signed update metadata matched the production public key.
  This diagnostic launcher identifies as v19. Its app code is unchanged in #136.

## Candidate identity

| Artifact | SHA256 |
| --- | --- |
| Unsigned launcher | `20f778f8b376daea413a48fcd3fe282ed177f7b08a9804dc6c95d0244ea07de4` |
| SHA256SUMS | `bbdf1d478dc0a15fd105cde47057ab85190510b59742ffabf8da1a94117d2d49` |
| Kernel | `a4671ce8cd648aaf5af83f9a65163c4d6c7083dd63099f16426bd1bd4f0c888f` |
| Initramfs | `ca3ada24326a80754499a1c3931846e8baec9e6210a52b2b59833291a3f53718` |
| Compressed rootfs | `b32a717cf771919a1fadb5d28d870c2f2cba2823a2c1a00cc4cafc9a8a08ccf9` |
| Raw rootfs | `1e8f520eba76f773650a186d8a05f5cd509b02436f2770293d82c3511a79abef` |
| Runtime ZIP | `8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc` |

The unsigned launcher defaults to public v19. Use the explicit local-asset
arguments in [LAPTOP-PASS.md](../LAPTOP-PASS.md) for candidate testing.

## Remaining before release

1. Confirm physical Windows sleep/wake and audio recovery. Normal use is accepted;
   no sleep/resume event has been recorded yet. Headphone switching has no
   separate explicit confirmation.
2. Merge the reviewed correction, pin the accepted payload, build the signed v20
   launcher through the protected workflow and repeat final acceptance.
3. Publish only after those checks pass. The user requested release once verified.

This is a preview milestone. Broader Windows/GPU coverage, portable lifecycle,
native migration and stable-version update paths remain v1 gates.
