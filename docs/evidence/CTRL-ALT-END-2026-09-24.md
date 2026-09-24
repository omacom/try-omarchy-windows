# Ctrl+Alt+End laptop acceptance, September 24, 2026

The unsigned Windows amd64 launcher candidate was built from branch
`codex/ctrl-alt-end-20260924`. Its SHA-256 is
`c7eaf7d5e1d9b835cb29c061cb3fac3187a369a736eb5c08a4787eb830aca1cd`. The
same hash was verified on the laptop for
`D:\TryOmarchy-CtrlAltEnd-20260924\TryOmarchy-ctrl-alt-end.exe`.

The test used a fresh copy of
`D:\TryOmarchy-v0.1.0-20260922\clean`, with source guest manifest
`3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`. It was
launched with `-dir` pointing to the copy, `-no-update`, a nonexistent `-winq`
path, `-start`, and guest SSH port `2253` with the existing
`v010_clean_guest_known_hosts` pin. The public v0.3.0 guest update was confirmed
at `06:36:24`; the keyboard-test boot reached userspace at `06:47:32`. QEMU ran
in session 1 as PID `25856`.

Hyprland was version `0.56.2`. The older `hyprctl dispatch exec foot` command
was rejected by its Lua dispatcher parser, so three `foot` windows were opened
with `hyprctl dispatch 'hl.dsp.exec_cmd("foot")'`. The guest boot ID was
`7a968d6e-55ca-4fbf-9f44-b122367d8e1d`; `systemctl is-system-running` returned
`running` with 0 failed units. Before testing, `hyprctl clients -j` counted 3
foot windows.

Each input task first called `SetForegroundWindow` for the QEMU window and then
checked `GetForegroundWindow` immediately before sending input. For the three
negative checks, the foreground PID was `25856`, equal to the QEMU PID, and the
extended-key flag was set on End:

- Plain End sent 2 events. The guest still had 3 clients, and no forwarded
  delete line appeared in `vm/shell.log`.
- Ctrl+End sent 4 events. The guest still had 3 clients, and no forwarded
  delete line appeared.
- Alt+End sent 4 events. The guest still had 3 clients, and no forwarded
  delete line appeared.

The positive Ctrl+Alt+End sequence sent 6 events: Ctrl down, Alt down, End down
and up, then Alt and Ctrl up. `SendInput` accepted all 6 events, and the
foreground PID immediately before input was still QEMU PID `25856`. The hook
log contained no `winkey: forwarded ctrl`, `winkey: forwarded alt`, or
`winkey: forwarded delete` lines. At `06:50:09`, `hyprctl clients -j` still
counted 3, the boot ID was unchanged, and systemd was `running`. The chord did
not close the windows. This is a failed positive acceptance check. The held-End
repeat case was not run after the failure.

A first injection-task attempt exited before the `SendInput` call because the
helper's User32 P/Invoke declarations omitted the native entry-point names. I
added the explicit names before the recorded negative and positive checks. The
first attempt produced no result file and no forwarded-key log entries. The
last guest diagnostic read at `06:51:16` also showed the ordinary Omarchy
screensaver client; the decisive positive result above was captured at
`06:50:09` before that idle overlay appeared.

The real Ctrl+Alt+Delete sequence was not synthesized. It remains reserved for
the Windows security screen. Physical keyboard use and Remote Desktop behavior
were not tested. The CSSI dialog was not clicked or sent keys. The guest was
powered off cleanly at `06:52:35`; the disposable copy, candidate executable,
scripts and scheduled tasks were removed. The source installation and Windows
settings were not changed. Small logs and input records remain in
`D:\TryOmarchy-CtrlAltEnd-20260924\evidence` and
`/tmp/ctrl-alt-end-evidence`.
