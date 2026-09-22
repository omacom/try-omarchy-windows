# Remote Windows laptop testing

The September 19 laptop acceptance work used Windows SSH, interactive scheduled
tasks, guest SSH and private QMP sockets. RustDesk was the fallback for Windows
sign-in and physical observation, not the main command interface.

## Local operator runbook

On the maintainer's Linux workstation, start with:

`~/.local/share/try-omarchy-laptop-control/README.md`

That machine-local runbook contains the verified connection details, host-key
fingerprint, installation/recovery map, exact commands, screenshot procedure and
known testing limitations. Its companion directory preserves the helper scripts
and public host-key pins outside `/tmp`. The existing private key remains in
`~/.ssh`; it is not copied into this repository or the helper archive.

The persistent `windows-ps.py` and `guest-ssh` helpers were both exercised after
being moved out of `/tmp`. The `tunnel` helper recreates the documented connection
when needed; check for an existing listener before running it.

## Control layers

1. **Windows SSH:** run PowerShell using an encoded command, inspect services and
   processes, transfer candidate builds with SCP and collect logs. Verify the live
   launcher and QEMU command lines before acting on an installation.
2. **Interactive scheduled tasks:** launch the app, inspect native windows,
   exercise controls and take screenshots in the signed-in user's desktop.
   An ordinary SSH process does not share that interactive desktop.
3. **Guest SSH:** reach the launcher's localhost-only forwarded guest port through
   an SSH tunnel. Inspect Linux services, compare file hashes, exercise capture
   and request clean poweroff.
4. **Private QMP:** use the tools socket for guest key input and screenshots when
   needed. Keep the launcher's supervisor and forwarding sockets separate. See
   `scripts/qmp.ps1` and `scripts/qmp-transport.ps1` for the transport implementation.

## Resume precautions

- Recheck host addresses, host keys, current processes and installation paths.
  Do not bypass host-key verification or reuse an old PID/window handle.
- Windows graphical work requires an interactive sign-in. Plan host reboots around
  SSH service startup and that sign-in; do not assume an unattended reconnect.
- Wait for a screenshot task to finish and confirm its output timestamp before
  reading the image. Confirm the result of UI actions, not just task exit status.
- Use DPI-aware coordinates for native clicks and activate the intended dialog
  before sending keys. Prefer control IDs or guest SSH where possible.
- Stop the guest and wait for QEMU to exit before disk mutation. A launcher may
  still be compacting after the guest stops. Preserve original installations and
  recovery archives.
- Use isolated test copies for destructive recovery scenarios. Physical tests,
  test-key update fixtures and exact signed-release acceptance prove different
  things. Record those boundaries with the results.

The [desktop candidate record](evidence/DESKTOP-POLISH-2026-09-19.md) and
[v1 laptop record](evidence/V1-LAPTOP-2026-09-19.md) describe what was actually tested.
