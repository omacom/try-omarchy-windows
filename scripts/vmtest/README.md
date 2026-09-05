# Windows VM test harness

These scripts drive a candidate launcher inside a Windows 11 VM without anyone
at the keyboard. They are what the v0.0.12 release checklist and the follow-up
launcher work were verified with. Nothing here runs in CI yet; it needs a host
with nested virtualization and a Windows guest.

## The VM

A [dockur/windows](https://github.com/dockur/windows) container with `VMX=Y`
and a Windows 11 guest. Windows 10 cannot run nested WHPX and hangs at boot
once the Hypervisor Platform is enabled, so use Windows 11. Give the container
8 GB and 4 cores; the launcher sizes the guest from what Windows can spare.
Expose the guest's OpenSSH server on a loopback port (2222 below) and mount a
host folder as `\\host.lan\Data` so files and screenshots move both ways.

GPU rendering never works under nested virtualization. Every launch falls back
to CPU rendering, and the launcher remembers that in `render-probe.json`.
Anything GPU, audio, sleep, DPI, or remote-session related needs physical
hardware; see `docs/RUNTIME-VALIDATION.md`.

## Serving the release payload

The launcher downloads without a GitHub token and a draft release's assets
require one, so serve the assets from the container instead:

```bash
docker exec -d omarchy-windows sh -c 'python3 -m http.server 18080 --bind 0.0.0.0 --directory /shared/candidate > /tmp/http.log 2>&1'
```

Inside the VM the container host is `172.30.0.1`, so the launcher takes
`-release http://172.30.0.1:18080/assets -sums-sha256 <digest>` and the
matching `-runtime-*` flags. `/tmp/http.log` in the container shows exactly
which assets each launch fetched, which is how the "no re-download" checks
are made.

## Running PowerShell in the VM

`winps.sh` runs a PowerShell script (file or stdin) over SSH without echoing
the password; it reads `PASSWORD` from the container's environment. Two traps:

- Multi-line `if`/`while` blocks over `powershell -Command -` stop silently.
  Keep control flow on one line, or end the script with a blank line.
- Every SSH PowerShell session opens a Windows Terminal window on the VM
  desktop that covers dialogs. Screenshots must minimize other windows and
  raise the target from inside the interactive task (`screenshot.ps1 -front`).

Anything that opens a window (the launcher, Settings, dialogs) must run as an
interactive scheduled task, not from the SSH session. `run-candidate.ps1` does
that for a launcher build and waits for the guest to report ready.

## Driving the guest

`qmp.ps1` talks to QEMU's tools QMP port (4445) from inside the VM:
`qmp.ps1 key meta_l,ret` opens a terminal in Omarchy, `qmp.ps1 type '...'`
and `qmp.ps1 key ret` run a command. Pair it with the shared folder: put a
script in `Omarchy Shared`, run it with `bash ~/Omarchy\ Shared/x.sh`, and
have it write its results back to the same folder. Nested SSH into the guest
stalls; keystrokes and the shared folder do not.

Never send keystrokes with `sendkeys.ps1` to the "Try Omarchy" title while
the VM window is up: they land inside the guest.

## Other helpers

- `sendkeys.ps1 -title T -keys K` answers a launcher prompt (Enter selects
  the highlighted option).
- `screenshot.ps1 -out file.png [-front "window title"]` captures the desktop.
- `movewin.ps1 -out file.txt [-read]` moves the VM window or reports its
  rectangle, for the window-placement checks.
- `clipimg.ps1 -out file.txt -set | -get | -text "..."` puts an image or text
  on the Windows clipboard, or reports what it holds.
- `sparsetool/` is a small Windows program (`go build` with `GOOS=windows`)
  that extracts a backup ZIP, copies a data folder, or hashes a tree while
  keeping sparse files sparse. The launcher's own restore budgets space from
  the compressed size, but this is still the fastest way to stage a copied
  install for a test.

## Checks worth repeating on every candidate

Backup and restore, in-place update with the compat repair, in-guest reboot
and poweroff, a second launch that downloads only `SHA256SUMS`, disk growth,
a fresh install through the location picker, and the forced rollback (kill
the launcher after "QMP connected" and before "userspace announced ready",
then relaunch and confirm no download and the previous payload).
