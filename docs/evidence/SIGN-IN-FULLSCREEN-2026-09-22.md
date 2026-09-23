# Windows sign-in launch acceptance — 2026-09-22

An unsigned candidate built from PR #158 plus its review fixes was tested on
the signed-in Windows 11 laptop in an isolated temporary installation. The
existing Omarchy installation and guest were not started or changed.

- Interactive Settings test enabled **Open fullscreen (Immersive)** and
  **Launch when I sign in to Windows**. Both preferences persisted.
- The current user's Startup folder received a link to that test
  installation's stable launcher with `-start` arguments. Disabling sign-in
  launch in Settings removed the link while leaving fullscreen enabled.
- The shortcut ownership and removal test passed 20 consecutive runs after a
  bounded retry was added for transient Windows file-sharing locks.
- A restored copy's local launch preference retained its value, while its
  sign-in launch preference was cleared before shortcuts were created.
- Linux Go tests and Windows cross-compilation passed. Native Windows tests
  `TestRestoredShortcutsTargetOnlyRestoredFolder`,
  `TestSignInShortcutOwnership`, and `TestSignInFullscreenSettingsNative`
  passed.

The test did not sign out of Windows. Acceptance verifies the Startup-folder
entry and Settings behavior; Windows runs that per-user entry after sign-in.
The temporary scheduled task, binaries, and sign-in link were removed.
