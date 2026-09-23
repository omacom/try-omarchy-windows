# Approved Windows apps in Omarchy

This feature is an **unreleased candidate** for [issue #160](https://github.com/omacom/try-omarchy-windows/issues/160). The public `v0.1.0` build does not include it.

In Windows **Try Omarchy Settings → Apps**, choose **Add Windows app...** and select a local `.exe`. Save Settings. A **Windows: App name** entry appears in Omarchy's launcher at startup or shortly after a running session synchronizes. Selecting the entry starts that approved program on Windows and minimizes the Omarchy window. Double-click the Try Omarchy tray icon, or choose **Open Omarchy**, to return. If Omarchy locked while Windows was in use, unlock it as usual.

The approval list stays in `approved-windows-apps.json` beside that installation's settings. Each entry has a random ID, display name and host path. The guest receives only the ID and name; it cannot supply an executable path, arguments, URL, or working directory. The host reloads the allowlist for every launch. Removing an app blocks new launches immediately and removes its guest entry on the next sync, at most about 30 seconds later. The host port is loopback only, one-shot requests are bounded, and launches are rate limited.

Approvals are intentionally excluded from backups. A restored copy on another PC starts with no approved Windows executables. Moving an installation on the same PC retains its approvals. If an approved program is removed, its launcher request fails with a guest notification until the entry is removed from Settings.

This first phase runs a normal Windows window outside Hyprland. It does not stream a single Windows window into the Linux desktop or guarantee game, anti-cheat, DRM or protected-content compatibility. The private agent path, Settings control, app launcher and fullscreen return flow were physically tested with Notepad on the AMD Windows 11 laptop in an isolated test installation. The [physical record](evidence/WINDOWS-APP-BRIDGE-2026-09-23.md) separates the older test disk with installed helpers from the new full guest image, which passed its own build and boot smoke. An exact signed candidate is still needed before release.
