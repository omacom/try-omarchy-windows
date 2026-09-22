# Focused parity follow-up, September 22, 2026

This pass refreshed the comparison against Mac source commit
`d843f54a37346dbb08ee4610785836194ca7e3bd` and added two bounded Windows
features that fit the existing launcher architecture.

Source commit: `e7bf0a6` on `codex/parity-master-integration`.

## Implemented

- General Settings can make owned Start-menu and Desktop launch shortcuts use
  `-start`. The separate Settings shortcut remains available. Portable launchers
  do not expose this host-shortcut behavior.
- A guest application named **Try Omarchy Settings** and the
  `omarchy-native-settings` command send a fixed `settings` request to the
  existing loopback-only launcher lifecycle listener. The running Windows
  launcher opens its native Settings process. The guest cannot provide host
  paths, commands or arguments.
- Compatibility revision 30 carries the guest command and desktop entry to
  existing persistent disks.

## Validation

- Linux Go and race suites: pass.
- Windows vet with the repository's existing unsafe-pointer exclusion: pass.
- Windows amd64 launcher and test binary cross-build: pass.
- Guest patch series and contract: 157 tests passed, one expected skip.
- Automatic startup uses a separate `launch-preferences.json`; the existing
  device-preferences schema stays unchanged for older-launcher rollback.
- Final native Windows execution remains recorded separately because it must run
  in the signed-in desktop session.

## Deliberately deferred

Live audio routing, bridged networking, Windows Hello sudo, host battery
mirroring and live guest-memory reclamation require new security, driver or
runtime work. They were not added as controls without working implementations.
Physical-finger pinch acceptance and broader GPU coverage remain hardware gates.
No release, runtime pin or active guest data changed during this source pass.
