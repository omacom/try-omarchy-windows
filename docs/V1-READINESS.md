# Road to v1

Baseline: [v0.0.20-preview](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.20-preview),
published September 19, 2026. The next milestone is a focused v1 release candidate.
The project remains a preview until the release gates below are satisfied.
For another session, start with [the current handoff](HANDOFF.md).

[Issue #77](https://github.com/omacom/try-omarchy-windows/issues/77) tracks delivery
and links to this document for scope and acceptance requirements. Record results
with [TESTING.md](TESTING.md), including the exact launcher, runtime and guest.
A code merge, automated test, physical test and public release are distinct evidence.

## Intended v1 scope

A dependable Omarchy desktop on x86_64 Windows, with persistent files, understandable
resource controls, clipboard and folder transfers, updates, backup and recovery,
and configuration export to a native Omarchy installation.

Windows 10 and 11 are currently advertised; each needs explicit acceptance before
v1 claims support. GPU acceleration depends on host drivers, with CPU rendering
as fallback. The app runs QEMU on Windows Hypervisor Platform, not a WSL distribution.

Portable mode remains experimental pending real installed-guest conversion,
external-drive lifecycle and second-PC testing. Webcam capture, accelerated RAM
resume, direct drops into arbitrary guest applications, bridged networking,
Windows ARM64 and booting an existing physical installation are outside the v1
commitment. Multiple display windows exist; that does not establish physical
mixed-DPI or multi-monitor input acceptance. Borderless mode is optional follow-up.

Before stable publication, document maintainer ownership, the issue-reporting
route and supported host combinations. Official positioning does not imply that
every Windows device or every native Omarchy feature has been validated.

## Shipped baseline and evidence

| Area | Evidence available | Remaining boundary |
| --- | --- | --- |
| Omarchy 4.0.3, browser-theme permissions, existing-guest package upgrades | [Guest upgrade validation](GUEST-UPGRADES.md), including preservation fixtures and busy-lock handling | Controlled orphaned-lock recovery passed in v20; interruption during package writes remains open |
| Installation moves, backup/restore, snapshots/rollback, growth/reclaim, clipboard and transfer windows | [Windows laptop acceptance](evidence/WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md) | Retest critical paths on the next exact candidate; portable lifecycle is unproven |
| Source-built graphics runtime and matching source pinned in v18 | Same acceptance record; final r15 Vulkan playback and preserved files | AMD laptop coverage does not establish Intel/NVIDIA or full Hyper-V support |
| GPU/CPU display paths, idle measurements and one-hour endurance | Same record, with individual runtime revisions identified | Earlier-runtime results are not exact-final-runtime acceptance for every check |
| Signed release preparation, publication and public download verification | Final publication sections of the acceptance record | Preview-to-stable migration is a separate gate |

The acceptance record is chronological: later checkpoints supersede earlier
failures only where a retest is explicitly recorded. v15–17 were unpublished
candidates; their reports remain historical supporting evidence.

## Work merged after v18

- [#113](https://github.com/omacom/try-omarchy-windows/pull/113): direct portable
  copying from raw disks and bounded retries for temporary Windows publication locks.
- [#114](https://github.com/omacom/try-omarchy-windows/pull/114): direct independent
  QCOW2 copies, including verified factory-backed sources.

- [#118](https://github.com/omacom/try-omarchy-windows/pull/118): runtime `4.0.3-4`
  repairs duplicate Neovim command ownership and preserves helpers during upgrade.
  Compatibility revision 21 delivers the corrected repository to existing guests.
  The complete factory build/boot and five-boot normal-updater regression pass;
  see [ownership repair evidence](evidence/RUNTIME-OWNERSHIP-2026-09-14.md).

- [#119](https://github.com/omacom/try-omarchy-windows/issues/119): the factory
  builder dropped the packaged `omarchy-nvim` theme symlink by rebuilding
  `/etc/skel/.config/nvim` from the package directory copy, which omits
  `lua/plugins/theme.lua`. Materialization now retains the package skeleton,
  compatibility revision 22 ships the link to existing disks, and `catch-up`
  restores it for users who lost it without replacing their own file. See
  [Neovim skeleton evidence](evidence/NVIM-SKELETON-2026-09-14.md).

- [#90](https://github.com/omacom/try-omarchy-windows/issues/90): an interrupted
  package update can leave an orphaned `/var/lib/pacman/db.lck` that blocks every
  later update with "unable to lock database" until it is removed by hand.
  `try-omarchy-pacman-lock.service` now runs once at early boot and removes the
  lock only when it provably cannot belong to a live transaction: a regular file,
  no pacman/alpm process, no process holding it open, and an mtime older than this
  boot. It logs the decision and never repairs the database. Compatibility
  revision 23 delivers it to existing disks.

The portable-copy, runtime-ownership and Neovim repairs above shipped in v19.
The orphaned-lock recovery, camera/microphone repairs, quiet file drops and complete
kernel-module delivery shipped in **v0.0.20-preview**. The
[September 19 evidence](evidence/PREVIEW20-CANDIDATE-2026-09-19.md) records fresh and
existing-guest Windows checks, preserved files, backup/restore, interrupted launcher
update rollback, signed acceptance and verified public downloads. Sleep/wake was
explicitly left untested for that preview. Large installed-guest portable lifecycle,
stable update chains and the broader checks below remain open.

## Remaining release gates

1. **Updates and recovery.** Preview 20 passed fresh installation, preserved
   existing-guest upgrades, controlled orphaned-lock recovery and interrupted
   launcher/payload rollback. The [original v7-to-v20 update and rollback](evidence/V1-LAPTOP-2026-09-19.md)
   also passed on physical Windows, closing the repository-transfer gate.
   Remaining work: interruption during package writes or power loss mid-extraction;
   both signed feeds across a stable release; an old preview that skips the bridge;
   preview-to-stable, direct stable, stable-to-stable and forced rollback.
   Record file checksums, versions, redirects and signatures. Launcher rollback
   does not undo installed guest OS package updates. Select the tested bridge
   preview in the release environment before stable publication; that variable
   was still unset at the September 19 checkpoint.
2. **Supported Windows hardware.** Obtain physical Intel and NVIDIA graphics
   results, full Hyper-V coexistence and Core Ultra virtualization coverage.
   Record Windows version/edition; include each advertised Windows version and
   lower-memory hardware. Complete sleep/resume, physical mixed-DPI movement,
   keyboard/focus and RDP/VNC checks, headphone switching and microphone-indicator
   observations. Repeat long-session and idle checks on the selected runtime.
3. **Storage regression.** V20 now has physical full-guest snapshot/restore and
   growth/no-shrink acceptance with preserved fixtures; see the [laptop pass](evidence/V1-LAPTOP-2026-09-19.md).
   Complete move, reset, active snapshot rollback and reclaim on the candidate. Include
   interruption, low space and temporary Windows file locks. Lowering capacity
   or rolling back the launcher must not shrink an existing disk.
4. **Native migration.** Restore an export onto a fresh physical Omarchy install;
   verify actual package/theme restoration and exclusion of VM-specific state.
5. **Release communication and distribution.** Align README and compatibility
   documentation with tested support; explain [guest OS updates](GUEST-UPGRADES.md)
   separately from launcher updates. Prepare stable notes and winget distribution,
   then verify public downloads, versions and signatures after publication using
   [RELEASING.md](RELEASING.md).

## Next checkpoint

Test the stable update chain, remaining storage cases and native migration while
collecting missing hardware and sleep/wake results. Keep evidence linked from #77. Publish a
stable candidate only through the normal release process; this document does not
authorize publication or certify the current preview as stable.
