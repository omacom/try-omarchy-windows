# Focused parity follow-up, September 22, 2026

This pass refreshed the comparison against Mac source commit
`d843f54a37346dbb08ee4610785836194ca7e3bd` and added two bounded Windows
feature that fits the existing launcher architecture.

Accepted implementation boundary: `655d6ac` on
`codex/parity-master-integration`. The earlier `e7bf0a6` attempt also contained
the in-guest Settings bridge that was later removed.

## Implemented

- General Settings can make owned Start-menu and Desktop launch shortcuts use
  `-start`. The separate Settings shortcut remains available. Portable launchers
  do not expose this host-shortcut behavior.

## Validation

- Linux Go and race suites: pass.
- Windows vet with the repository's existing unsafe-pointer exclusion: pass.
- Windows amd64 launcher and test binary cross-build: pass.
- Guest patch series and contract: 157 tests passed, one expected skip.
- Automatic startup uses a separate `launch-preferences.json`; the existing
  device-preferences schema stays unchanged for older-launcher rollback.
- Final native Windows execution remains recorded separately because it must run
  in the signed-in desktop session.

The exact unsigned follow-up candidate passed 381 native Windows tests with 32
expected skips. `TestAutomaticStartSettingNative` opened the real Win32 Settings
window, enabled the control, saved it and verified the separate preference file.

## Deliberately deferred

Live audio routing, bridged networking, Windows Hello sudo, host battery
mirroring and live guest-memory reclamation require new security, driver or
runtime work. They were not added as controls without working implementations.
Physical-finger pinch acceptance and broader GPU coverage remain hardware gates.
No release or runtime pin changed. The retained guest was booted once for the
live request check, then powered off cleanly without reset or recovery actions.

An in-guest request reached the running host launcher and started a Settings
child process, but the native window did not become visible. That implementation
and its guest patch were removed rather than retaining an unaccepted control.
