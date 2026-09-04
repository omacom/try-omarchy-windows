# v1 readiness

The Windows app and the Mac app now live under Omacom. A stable Windows release
still needs the checks below. A passing build does not establish hardware or
upgrade reliability.

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
- [ ] Agree release ownership, signing access and recovery, branding, download
  location, support routing, and shared Mac/Windows behavior with maintainers (#37).
- [ ] Update user documentation to describe the tested release, including how
  existing guests receive Omarchy OS updates separately from launcher updates.

Use [TESTING.md](TESTING.md) for reports. Each gate needs evidence for the exact
candidate version and runtime, not only an earlier preview. Keep release
publication separate from code review and merging.

## Scope

v1 should provide a dependable way to try Omarchy, keep a trial setup, and take
its configuration to a full installation. Prioritize reliability, storage,
recovery, and understandable controls.

Image clipboard and better file transfers can follow the core work. Camera
bridging, Windows ARM64, and additional portable launchers remain later work.
Booting an existing physical installation is outside the v1 scope.
