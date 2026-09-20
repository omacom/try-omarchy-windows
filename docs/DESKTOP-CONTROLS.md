# Desktop controls in the next candidate

These controls are implemented in the development branch. The published download
is still v0.0.20-preview; it does not yet include these Settings changes.

Settings has four pages:

- **General:** fullscreen, memory in GB, disk capacity, installation location and shared folder.
- **Devices:** camera selection, camera access and microphone access, plus Windows privacy settings links.
- **Advanced:** guest displays, rendering, CPUs, port forwards, SSH and automatic launcher update checks.
- **Recovery:** backup, restore, snapshots, reset, move, cleanup, portable copy and uninstall.

Save, then restart Omarchy to apply changes. Automatic resources remain available
by entering 0. A smaller disk setting never shrinks an existing disk.

Camera selection uses the Windows device identity, not its position in a list.
If the chosen camera is disconnected, capture stays unavailable instead of
silently switching to a different camera. Automatic selects the first available
camera. Cameras open only on a guest capture request; opening Settings enumerates
devices without activating them. The tray's Camera status reports idle, active,
disabled or the last capture error. Windows camera permission is still required.

Turning microphone access off prevents QEMU from opening host recording voices
while keeping playback enabled. Recording uses the Windows default device when
enabled. Both device switches retain the existing enabled behavior unless changed.

About and updates is available from Settings and the tray. Manual checks verify
the signed release metadata and offer the release notes/download page when a
newer compatible version exists. They do not replace a running launcher. Automatic
checks can be disabled in Advanced; Linux package updates remain separate inside
Omarchy under Update > Omarchy.

Device and update choices live in `desktop-preferences.json`. Older launchers can
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
exchanges and links. Direct drops into arbitrary guest applications remain outside
v1 scope.

See the [candidate verification record](evidence/DESKTOP-POLISH-2026-09-19.md).
