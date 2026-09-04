# v1 readiness

The Windows app and the Mac app now live under Omacom. A stable Windows release
still needs the checks below. A passing build does not establish hardware or
upgrade reliability.

## Official v1 checklist

These items follow the Omarchy Basecamp board. Keep technical work moving while
maintainer decisions are pending; those decisions are not prerequisites for
code changes or hardware testing.

| Board item | Current evidence and remaining work |
| --- | --- |
| Restore signing and release CI after the transfer | The [post-transfer signing check](https://github.com/omacom/try-omarchy-windows/actions/runs/33832168714) passed, including Authenticode and the update signing key. Still verify draft preparation and publication through the transferred repository when the candidate is ready. See [RELEASING.md](RELEASING.md). |
| Decide official ownership of signing and hosting | Maintainer decision in #37. Keep separate from the technical work below. |
| Validate the source-built QEMU runtime on real hardware | Record the exact archive hash and physical Windows results in #12 and #10 using [RUNTIME-VALIDATION.md](RUNTIME-VALIDATION.md). |
| Promote the source-built QEMU runtime into production | After validation, pin the tested runtime and matching source archive in `guest-build/runtime.lock.json`, prepare a candidate, and verify its packaged runtime. Build success alone does not complete #12. |
| Verify the first update across the repo transfer | Test a copied pre-transfer installation through the signed update path into the Omacom candidate. Check redirects, downloads, preserved files, and rollback in #14. This is separate from preview-to-stable migration. |
| Validate full Hyper-V compatibility | Run the full launch, rendering fallback, clipboard, sharing, audio, reboot, and shutdown checks on physical Windows with full Hyper-V enabled. Record results in #4 and update the compatibility documentation. |
| Publish v1.0 through the official sites | Prepare concise release notes and verified Windows download links. After maintainer approval and publication, check the official site links, downloaded signature, and version. Site access and publication approval remain with maintainers (#37). |

## Next technical work

1. Validate the source runtime on physical hardware, including full Hyper-V.
2. Promote the exact tested archives and prepare a signed candidate.
3. Test fresh installs, the repository-transfer update, preview-to-stable
   migration, rollback, and data preservation.
4. Finish the applicable release gates below and prepare the release notes and
   official download handoff. Publish only after validation and approval.

## Merged for release testing

- [#34](https://github.com/omacom/try-omarchy-windows/pull/34): stable updates,
  including a bridge for installations that skip preview releases.
- [#35](https://github.com/omacom/try-omarchy-windows/pull/35): disk capacity,
  in-place growth, and Windows free-space information.
- [#29](https://github.com/omacom/try-omarchy-windows/pull/29): official artwork,
  current resources, and splash icon handling.
- First-run install-location selection is on master and needs release testing.

## Release gates

- [ ] Reproduce and resolve the configuration report in #32, or document its
  confirmed cause and supported fix.
- [ ] Test the signed candidate on the Windows hardware matrix in #10, including
  the source runtime (#12), full Hyper-V (#4), and remote input (#3).
- [ ] Record fresh install, existing guest upgrade, interrupted update, and
  forced rollback (#14). Include an old preview that skips the bridge and
  reaches stable through both signed update feeds.
- [ ] Verify disk growth inside the guest, unchanged user files, and unchanged
  capacity after lowering the setting or rolling back the launcher (#15).
- [ ] Restore a configuration export onto a fresh physical Omarchy install (#5).
  Confirm package and theme restoration and exclusion of VM-specific state.
- [ ] Finish stopped-VM backup/restore and a clear reset flow (#36) before presenting
  the guest as suitable for persistent work. Configuration export is not a VM
  backup.
- [ ] Exercise sleep/resume, mixed-DPI resize, audio-device changes, and a long
  session. Record idle CPU and whether launch alone activates the microphone.
- [ ] Update user documentation to describe the tested release, including how
  existing guests receive Omarchy OS updates separately from launcher updates.

Use [TESTING.md](TESTING.md) for reports. Each gate needs evidence for the exact
candidate version and runtime, not only an earlier preview. Keep release
publication separate from code review and merging.

## Maintainer coordination

Signing ownership, key recovery, hosting, official site access, release approval,
and support ownership are tracked in #37. Record decisions when available;
continue implementation and validation without waiting for them. Prepare the
release assets, notes, and download handoff within this repository first.

## Scope

v1 should provide a dependable way to try Omarchy, keep a trial setup, and take
its configuration to a full installation. Prioritize reliability, storage,
recovery, and understandable controls.

Image clipboard and better file transfers can follow the core work. Camera
bridging, Windows ARM64, and additional portable launchers remain later work.
Booting an existing physical installation is outside the v1 scope.
