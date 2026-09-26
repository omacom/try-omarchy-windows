# Desktop controls

This guide describes the published `v0.4.0` release.

Settings has four pages:

- **General:** fullscreen and monitor selection, automatic startup, CPU/RAM profiles, disk capacity, installation location and shared folder.
- **Devices:** camera selection and access, microphone access, live playback/recording choices with the bundled r20c runtime, plus Windows privacy and sound settings links.
- **Advanced:** guest displays, rendering, port forwards, SSH and automatic launcher update checks.
- **Recovery:** backup, restore, snapshots, reset, move, cleanup, portable copy and uninstall.

CPU, memory, graphics, display, and microphone-access changes apply on the next
VM start. Audio device choices switch live with the bundled runtime. Choose the
Balanced resource profile for automatic sizing. A smaller disk setting never
shrinks an existing disk.

An ordinary launch opens these pages before the desktop. **Launch Omarchy** saves preferences and starts the guest;
**Close** leaves the guest stopped. The separate Settings shortcut still saves
without launching. Runtime command-line options keep direct startup; `-start`
explicitly bypasses the menu and `-launcher` explicitly opens it. Command-line
resource overrides still take precedence over saved settings for that launch.

When **Start automatically from Windows shortcuts** is enabled, owned Start-menu
and Desktop launch shortcuts use `-start`. The separate Settings shortcut still
opens the native controls.
**Launch when I sign in to Windows** adds a shortcut to the current user's
Startup folder and starts Omarchy directly after Windows sign-in. Combine it
with **Open fullscreen (Immersive)** to enter Omarchy fullscreen on launch.
Turning it off removes this installation's Startup shortcut. Windows still
shows its sign-in screen before this per-user startup runs.
Restored copies start with sign-in launch disabled so a restore beside the
original installation cannot start both copies at the next sign-in.
See [the Mac parity tracker](MAC-PARITY.md) for implementation and test boundaries.

While the Omarchy window is focused, Ctrl+Alt+End sends Ctrl+Alt+Delete to
Omarchy, since Windows keeps Ctrl+Alt+Delete for its own security screen. Pinch
to zoom on a Windows 11 Precision Touchpad reaches Linux apps in GPU mode with
one display. It has no Settings control; start with `-disable-pinch` to keep
ordinary two-finger input. See [trackpad pinch](PINCH-ZOOM.md).

Camera selection uses the Windows device identity, not its position in a list.
If the chosen camera is disconnected, capture stays unavailable instead of
silently switching to a different camera. Automatic selects the first available
camera. Cameras open only on a guest capture request; opening Settings enumerates
devices without activating them. The tray's Camera status reports idle, active,
disabled or the last capture error. Windows camera permission is still required.

Turning microphone access off takes effect at the next VM start and prevents
recording while keeping playback enabled. With the bundled r20c runtime,
**Sound output** and **Microphone** choices switch live from host Settings or
Omarchy's guest audio picker and persist across guest reboots. Older external
runtimes may apply choices only at startup or use Windows defaults.
**Windows sound devices** opens the host's sound settings. See
[audio device behavior and validation](AUDIO-DEVICES.md) for fallback behavior
and the remaining physical-device checks.

About and updates is available from Settings and the tray. Manual checks verify
the signed release metadata and offer the release notes/download page when a
newer compatible version exists. They do not replace a running launcher. Automatic
checks can be disabled in Advanced; Linux package updates remain separate inside
Omarchy under Update > Omarchy.

Camera, microphone-access and update choices live in `desktop-preferences.json`.
Automatic shortcut startup lives in `launch-preferences.json`, so older
launchers can ignore it safely after a rollback.
Audio routes live separately in `audio-preferences.json`, with stable Windows IDs
in `audio-endpoints.json`. Older launchers can
still read `settings.json` after rollback. Older launchers do not implement the
new device restrictions; if you deliberately run an older release, use Windows
privacy settings to block access. Backups, snapshots and portable copies include
the new preferences.

## File transfer behavior

Current guest integrations stream file and folder clipboard transfers and drops
with limits of 100 GiB and 10,000 entries, subject to free space on both systems.
The old 16 MiB embedded clipboard format remains a compatibility fallback, not the
current streaming limit. Images on the clipboard still have a 16 MiB limit.
Transfers always copy, including a Windows Cut selection. The source is retained.
A small progress window appears for longer transfers; closing it dismisses the
window, while Cancel stops the copy. Transfer errors use non-blocking Windows
notifications, so an early failed drop cannot hold up later transfers. Shared folders are useful for repeated large
exchanges and links. Direct drops into arbitrary guest applications are not yet implemented.

See the [candidate verification record](evidence/DESKTOP-POLISH-2026-09-19.md).
