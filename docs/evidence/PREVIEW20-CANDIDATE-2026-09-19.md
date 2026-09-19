# Preview 20 candidate, September 19, 2026

Guest and unsigned launcher source: `946521b4afeab02a6dcd6e3d8f02acc37553c507`,
[PR #136](https://github.com/omacom/try-omarchy-windows/pull/136).
Guest compatibility: **29**. [v0.0.20-preview](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.20-preview)
was published and promoted to Latest on September 19 at 21:38 UTC.
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
- The final v20 signing check from `98cce48131f1b0efa057c46d560a63cbb1bbfa3e`
  passed in [run 35470601017](https://github.com/omacom/try-omarchy-windows/actions/runs/35470601017).
  Windows verified Authenticode and version `v0.0.20-preview`; the update metadata
  signature matched the production key. The signed launcher reached the GPU
  desktop, captured 30 distinct camera frames, and delivered a native file drop
  without overwriting the existing file. Reboot reached a new guest boot ID,
  retained all fixture hashes and left both completed-repair timestamps unchanged.
  Test launcher SHA256:
  `73ad933ef71efa4da157149aa921f264463c2b26ae48eb2bed9010a906fc1e06`.

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

## Publication and remaining validation

Physical Windows sleep/wake remains **untested**. On September 19 the user
explicitly declined that physical test and accepted proceeding with this preview.
Normal use is accepted; headphone switching has no separate explicit confirmation.
This does not count as sleep/wake acceptance for v1.

The correction merged as `e1341df`; `98cce48` pins the accepted payload. Final
signed v20 acceptance passed. [Publication run 35470832033](https://github.com/omacom/try-omarchy-windows/actions/runs/35470832033)
passed Windows race tests, signing, payload verification, tagged and Latest
downloads, and both signed update feeds through the current and legacy repository
URLs. The public manifest matches the embedded pin. Both public metadata signatures
verified locally, and Windows verified the downloaded launcher signature and version.
The public Latest executable then launched with default public payload URLs,
reached the accelerated desktop, preserved all three fixtures and both repair
marker timestamps, and committed the payload only after userspace readiness.
The release commit CI rerun also passed once the manifest became public.

Published launcher SHA256:
`bef22fda3b22ce666dc195725be4e7f956945b1817c0af9f9912d2a15393c6dd`.
The publish workflow rebuilt from the same `98cce48` source; its hash differs from
the signing-check artifact because of build metadata and a new signing timestamp.

This is a preview milestone. Broader Windows/GPU coverage, portable lifecycle,
native migration and stable-version update paths remain v1 gates.
