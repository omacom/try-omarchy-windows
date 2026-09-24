# v0.3.0 signed candidate on the Windows laptop (September 24, 2026)

The Release prepare run [35958752529](https://github.com/omacom/try-omarchy-windows/actions/runs/35958752529) created draft `v0.3.0`. Its `SHA256SUMS` digest is `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. The supplied assets include runtime r20c.

Signing check [35959572969](https://github.com/omacom/try-omarchy-windows/actions/runs/35959572969) produced the launcher from commit `a32482f3e9061b1ed5535dc51e476a21d702cb96`. On the laptop, the transferred file had SHA-256 `0bba36e502ec72b0ff1370cdb61fb791a81f5bdcd20ba0a9d600ce265b90c03d`. Windows reported Authenticode `Valid`, signer subject `CN=Brandon South, O=Brandon South, L=Wilmore, S=ky, C=US`, FileVersion `v0.3.0`, and ProductVersion `v0.3.0`.

## Source installation and copy

The install inventory found no guest receipt matching the public `v0.2.0` manifest digest `597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`. I used the clean `v0.1.0` source at `D:\TryOmarchy-v0.1.0-20260922\clean`. Its receipt identified manifest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`.

The source was copied to `D:\TryOmarchy-v0.3.0-20260924\copy1-upgrade`. Robocopy reported success, and both trees contained 223 files with 34,185,909,824 logical bytes. The original installation was not booted or modified. The disposable copy was removed during cleanup. The work folder retains the evidence files.

## Guest connection and preserved data

Guest SSH used `StrictHostKeyChecking=yes` and the source-specific pin in `v010_clean_guest_known_hosts`. The verified server fingerprint was `SHA256:tbu1q3e2hn1FNRpMUhjreYWsrK8BusyGBuEABuXwCpI`. `systemctl is-system-running` returned `running`, the failed unit count was 0, and `~/Documents/v010-clean-marker.txt` matched the expected SHA-256 `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`.

## Physical upgrade and boot

The signed candidate launched copy 1 with the draft payload served at `http://127.0.0.1:18080` and the v0.3.0 manifest digest pinned. The guest receipt recorded that URL and digest. The launcher logged `guest update v0.3.0 confirmed after userspace reported ready`. No separate runtime update confirmation line appeared in `vm/shell.log`; the runtime receipt recorded the same manifest digest and the installed QEMU executable SHA-256 matched r20c at `44e6e56f88fcac5567bfc3995aa5113135efe18b22cd2d0316eb30819c254447`.

The QEMU command line included `virtio-vga-gl` and `-audiodev sdl`. The QEMU SDL window was visible, but the Windows screenshot did not show a rendered Omarchy desktop. The QMP screendump returned `no surface`. The screenshot is `D:\TryOmarchy-v0.3.0-20260924\evidence\candidate-copy1-desktop.png` and is also retained at `/tmp/v030/evidence/candidate-copy1-desktop.png`. The guest later powered off at 01:22:48 without a poweroff request from this test, and the launcher exited. The GPU desktop acceptance did not pass, so testing stopped here.

## Live audio

Not run. The bridge service, PipeWire endpoint list, session-1 playback and capture probes, route-file changes, and audio-choice restoration were not tested. The laptop has one physical endpoint per direction, so two-endpoint switching and hotplug remain untested. No human listening test was performed.

## Guest reboot and second launch

Not run as planned. The guest powered off unexpectedly during the first candidate session. A guest reboot, saved-choice persistence, and a no-download second launch were not tested.

## Interrupted-update recovery

Not run. Copy 2 was not created, and no interrupted update was attempted.

## Public release verification

Pending publication.
