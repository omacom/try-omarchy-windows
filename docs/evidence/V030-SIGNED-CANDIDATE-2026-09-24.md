# v0.3.0 signed candidate on the Windows laptop (September 24, 2026)

The Release prepare run [35958752529](https://github.com/omacom/try-omarchy-windows/actions/runs/35958752529) created draft `v0.3.0`. Its `SHA256SUMS` digest is `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. The supplied assets include runtime r20c.

Signing check [35959572969](https://github.com/omacom/try-omarchy-windows/actions/runs/35959572969) produced the launcher from commit `a32482f3e9061b1ed5535dc51e476a21d702cb96`. On the laptop, the transferred file had SHA-256 `0bba36e502ec72b0ff1370cdb61fb791a81f5bdcd20ba0a9d600ce265b90c03d`. Windows reported Authenticode `Valid`, signer subject `CN=Brandon South, O=Brandon South, L=Wilmore, S=ky, C=US`, FileVersion `v0.3.0`, and ProductVersion `v0.3.0`.

## Source installation and copy

The install inventory found no guest receipt matching the public `v0.2.0` manifest digest `597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`. I used the clean `v0.1.0` source at `D:\TryOmarchy-v0.1.0-20260922\clean`. Its receipt identified manifest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`.

Copy 1 was made at `D:\TryOmarchy-v0.3.0-20260924\copy1-upgrade`. Robocopy reported success, and both trees contained 223 files with 34,185,909,824 logical bytes. The original installation was not booted or modified. The disposable copy was removed during cleanup. Evidence remains under `D:\TryOmarchy-v0.3.0-20260924\evidence` and `/tmp/v030/evidence`.

## Guest connection and preserved data

Guest SSH used `StrictHostKeyChecking=yes` and the source-specific pin in `v010_clean_guest_known_hosts`. The verified server fingerprint was `SHA256:tbu1q3e2hn1FNRpMUhjreYWsrK8BusyGBuEABuXwCpI`. `systemctl is-system-running` returned `running`, the failed unit count was 0, and `~/Documents/v010-clean-marker.txt` matched the expected SHA-256 `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`.

## Physical upgrade and boot

The signed candidate launched copy 1 with the draft payload served at `http://127.0.0.1:18080` and the v0.3.0 manifest digest pinned. The guest receipt recorded that URL and digest. The launcher logged `guest update v0.3.0 confirmed after userspace reported ready`. No separate runtime update confirmation line appeared in `vm/shell.log`; the runtime receipt recorded the same manifest digest and the installed QEMU executable SHA-256 matched r20c at `44e6e56f88fcac5567bfc3995aa5113135efe18b22cd2d0316eb30819c254447`. The QEMU command line included `virtio-vga-gl` and `-audiodev sdl`.

## Screenshot timing and desktop state

Guest readiness was logged at `01:53:54`. The session-1 screenshot `candidate-copy1-desktop-rerun.png` was captured at `01:57:30`, but its in-image Windows taskbar clock reads `1:06 AM`, so it is stale and not used as guest evidence. After one wake attempt, `candidate-copy1-desktop-postwake.png` was captured at `02:01:39`; its in-image taskbar clock reads `2:01 AM`, which is later than readiness. This fresh screenshot shows the Try Omarchy QEMU window title, a black client area with small white blocks, and the CSSI reboot dialog. No Omarchy desktop is visible in the QEMU window. This meets the display failure condition, so checks C, D, and E were not run. The earlier `candidate-copy1-desktop.png` also showed `1:06 AM` before QEMU started at `01:15:09` and is not guest evidence.

The first wake task attempt returned result 1 without a result file, so it is unknown whether it emitted input. A corrected task reported one relative `SendInput` move of 1 by 1 at `02:01:24`. No further input was sent, and no CSSI dialog control was clicked. The QMP screendump limitation on the GL path was not used as display evidence.

## Review of the earlier 01:22 poweroff

The task that wrote `candidate-qemu-window-rect.json` at `01:22:13` only called `FindWindow`, `GetWindowRect`, and `GetWindowThreadProcessId`. The QMP command in that interval was `screendump` only, not `system_powerdown` or `send-key`. Screenshot tasks only called `CopyFromScreen`. An `AppActivate` attempt could change QEMU window focus but sent no keys or close command. The guest SSH probes ran read-only systemd and marker-hash checks. No `Alt+F4`, `WM_CLOSE`, guest SSH poweroff, or `SendInput` occurred in that interval. I found no action in that interval that requested guest poweroff, and its cause remains unidentified. The unexpected poweroff did not recur in this rerun.

After the valid screenshot showed no Omarchy desktop, the guest was powered off at `02:02:36` with `sudo systemctl poweroff` for cleanup. The requested `loginctl list-sessions`, `pgrep -a Hyprland`, `pgrep -a sddm`, and warning-level journal diagnostics were not collected before this shutdown. Earlier checks confirmed systemd was `running`, no failed units, and the marker hash matched. The copy was removed during cleanup, so the requested journal commands cannot be run against that boot now.

## Live audio

Not run. The bridge service, PipeWire endpoint list, session-1 playback and capture probes, route-file changes, and audio-choice restoration were not tested. The laptop has one physical endpoint per direction, so two-endpoint switching and hotplug remain untested. No human listening test was performed.

## Guest reboot and second launch

Not run. Guest reboot persistence and a no-download second launch were not tested.

## Interrupted-update recovery

Not run. Copy 2 was not created, and no interrupted update was attempted.

## Public release verification

Pending publication.
