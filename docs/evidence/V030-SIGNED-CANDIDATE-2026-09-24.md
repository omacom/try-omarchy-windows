# v0.3.0 signed candidate on the Windows laptop (September 24, 2026)

The Release prepare run [35958752529](https://github.com/omacom/try-omarchy-windows/actions/runs/35958752529) created draft `v0.3.0`. Its `SHA256SUMS` digest is `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. The supplied assets include runtime r20c.

Signing check [35959572969](https://github.com/omacom/try-omarchy-windows/actions/runs/35959572969) produced the launcher from commit `a32482f3e9061b1ed5535dc51e476a21d702cb96`.

## A. Signed launcher

The transferred launcher SHA-256 was `0bba36e502ec72b0ff1370cdb61fb791a81f5bdcd20ba0a9d600ce265b90c03d`. Windows reported Authenticode `Valid`, signer subject `CN=Brandon South, O=Brandon South, L=Wilmore, S=ky, C=US`, FileVersion `v0.3.0`, and ProductVersion `v0.3.0`.

## Source installation and copies

The laptop inventory found no guest receipt matching the public `v0.2.0` manifest digest `597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`. Copy 1 and rollback copy 2 were made from the clean `v0.1.0` source at `D:\TryOmarchy-v0.1.0-20260922\clean`, whose receipt identified manifest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`.

Each Robocopy completed with exit code 1, meaning files were copied. Each source and destination contained 223 files with 34,185,909,824 logical bytes. The original installation was not booted or modified. Copy 1 and copy 2 were removed during cleanup.

Guest SSH used `StrictHostKeyChecking=yes` and the source-specific pin in `v010_clean_guest_known_hosts`. The verified fingerprint was `SHA256:tbu1q3e2hn1FNRpMUhjreYWsrK8BusyGBuEABuXwCpI`. The preserved file `~/Documents/v010-clean-marker.txt` retained SHA-256 `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade` after upgrade and rollback recovery.

## B. Upgrade and GPU desktop

The signed launcher upgraded copy 1 using the draft payload at `http://127.0.0.1:18080` and manifest digest `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. The launcher logged `02:42:58 guest update v0.3.0 confirmed after userspace reported ready`. No separate runtime confirmation line appeared in `vm/shell.log`; the runtime receipt recorded the draft digest and the installed QEMU executable SHA-256 matched r20c at `44e6e56f88fcac5567bfc3995aa5113135efe18b22cd2d0316eb30819c254447`. The QEMU command line included `virtio-vga-gl` and `-audiodev sdl`. The absent separate runtime log line did not block acceptance because the runtime receipt and executable hash matched.

Captures taken after the guest's idle screensaver/display-off timeout were not used as display evidence. The Omarchy shell defaults in `/usr/share/omarchy/config/omarchy/shell.json` list a 150-second screensaver and a 300-second lock. No per-user `hypridle.conf` was present and `hypridle.service` was inactive. Hyprland reported `dpmsStatus=false` in a later idle query.

To obtain a live desktop image, the guest was rebooted at `02:59:45`; the launcher logged `02:59:48 guest rebooted - relaunching`. After userspace announced ready at `03:00:10`, the session-1 Windows screenshot was captured at `03:00:47`, 37.7 seconds later. Its in-image taskbar clock reads `3:00 AM`. The screenshot shows the Try Omarchy window with the Omarchy wallpaper and bar. The CSSI reboot dialog covers the center of the image. No button was clicked and no key was sent to it. The screenshot is `D:\TryOmarchy-v0.3.0-20260924\evidence\candidate-copy1-desktop-postreboot.png` and `/tmp/v030/evidence/candidate-copy1-desktop-postreboot.png`.

A guest `grim` capture at `03:01:41` also shows the Omarchy wallpaper and bar; the in-image bar clock reads `Thursday 03:01`. `hyprctl monitors -j` identified `Virtual-1`, resolution `1366x697`, with `dpmsStatus=true`; the Wayland socket was `wayland-1`. The image is `D:\TryOmarchy-v0.3.0-20260924\evidence\candidate-copy1-grim-postreboot.png` and `/tmp/v030/evidence/candidate-copy1-grim-postreboot.png`. Together, the Windows image and the guest capture pass the GPU desktop check.

After userspace came up, `systemctl is-system-running` returned `running`, failed unit count was 0, and the preserved marker hash matched the expected value.

## C. Live audio

`omarchy-windows-audio-bridge.service` was active and enabled. PipeWire listed Windows playback endpoints `Windows System Default` and `Speaker (Realtek(R) Audio)`, and recording endpoints `Windows System Default` and `Microphone Array (AMD Audio Device)`. The original app audio preference files were absent and both live route files contained `default`.

The session-1 Windows audio probe found no QEMU playback session in three idle samples. During a 15-second guest `pw-play`, five samples showed an active QEMU playback session on `Speaker (Realtek(R) Audio)`. Three samples after playback ended showed the session inactive. The test used a generated silent waveform, so no human listening test was performed.

With microphone access enabled in Settings, a 15-second `pw-record` capture produced 2,880,044 bytes. Five session-1 probe samples showed active QEMU recording on `Microphone Array (AMD Audio Device)`. The temporary recording was deleted after the byte count was recorded.

The guest's built-in Omarchy output switcher was exercised with `omarchy-audio-output-switch`. Selecting Windows System Default set the host preference and live route to default. Selecting `Speaker (Realtek(R) Audio)` saved that name and its stable endpoint ID on the host and changed the live output route. The switcher also cycled through the built-in Virtio QEMU sink; that is not a Windows endpoint and did not replace the last valid Windows route.

The signed candidate's Settings window was opened with `-winq` pointing to the nonexistent external-runtime path so its bundled r20c controls were used. The enabled output choices were Windows default and `Speaker (Realtek(R) Audio)`. The microphone choices were Windows default and `Microphone Array (AMD Audio Device)`, and Allow microphone access was checked. Choosing Windows default in Settings changed the guest default and live route to Windows System Default. Choosing the Realtek speaker in Settings changed the guest default and live route back to that speaker. A first Settings invocation without the `-winq` override was discarded; the repeat with the bundled-runtime selection applied the live route correctly.

After the persistence checks below, the original output and input choices were restored to Windows System Default. The preference values were blank, the endpoint IDs were empty, both live route files were `default`, and the guest defaults were Windows System Default. No Windows system audio defaults or privacy settings were changed. One physical endpoint per direction was available, so two-endpoint switching and hotplug remain untested.

## D. Guest reboot, persistence, and second launch

With the Realtek speaker selected, the guest logged `03:23:02 guest announced reboot` and the launcher logged `03:23:05 guest rebooted - relaunching`. On the rebooted guest, the audio bridge remained active and enabled, and the default output remained `Speaker (Realtek(R) Audio)` with the same host preference and live route. The system was `running`, failed unit count was 0, and the marker hash was unchanged.

A clean `sudo systemctl poweroff` at `03:24:08` was followed by launcher exit. The signed candidate was then started again on the same copy. Userspace announced ready at `03:25:09`; the audio bridge was active and enabled and the Realtek choice persisted. The guest and runtime receipts remained on the v0.3.0 digest, `guest.next` and `runtime.next` were absent, and there were no compatibility-repair lines on the second launch. The payload server recorded only a `SHA256SUMS` request for this launch, with no guest or runtime asset request.

The original Windows-default output and input choices were restored in Settings while the VM was running, and the guest defaults and route files returned to Windows System Default and `default`. The final clean `sudo systemctl poweroff` was logged at `03:27:45`, followed by launcher exit.

## E. Interrupted-update rollback

Rollback copy 2 was a separate fresh copy of the same clean `v0.1.0` source. It began with source manifest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9` and the original marker file.

During its candidate upgrade, the copy recorded v0.3.0 guest and runtime payloads as pending. GPU boot began at `03:47:42`. At `03:47:46`, before any `guest userspace announced ready` line, the test stopped launcher PID `23932` and QEMU PID `28444`. The update state showed both guest and runtime pending, and the source guest and runtime were retained as previous copies.

The recovery launch logged `03:50:23 using restored guest and runtime for this recovery launch` and reached userspace at `03:50:51`. Both restored receipts matched the original v0.1.0 digest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`. The marker hash still matched `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`, systemd was `running`, failed unit count was 0, and neither `.next` payload directory remained. The server recorded no payload request during recovery. A clean poweroff at `03:51:46` was followed by launcher exit.

## Review of the earlier 01:22 poweroff

The task that wrote `candidate-qemu-window-rect.json` at `01:22:13` only queried `FindWindow`, `GetWindowRect`, and `GetWindowThreadProcessId`. The QMP command in that interval was `screendump` only, not `system_powerdown` or `send-key`. Screenshot tasks called `CopyFromScreen`. An `AppActivate` attempt could change QEMU focus but sent no keys or close request. Guest SSH probes were read-only systemd and marker-hash checks. No `Alt+F4`, `WM_CLOSE`, guest SSH poweroff, or `SendInput` occurred in that interval. I found no action that requested that poweroff, and its cause remains unknown. No unexpected poweroff occurred during this acceptance run.

## Cleanup and scope

The original source installation and Windows settings were not changed. The disposable copies, candidate processes, scheduled tasks, payload server, and SSH tunnels were removed or stopped. Evidence remains under `D:\TryOmarchy-v0.3.0-20260924\evidence` and `/tmp/v030/evidence`. No release was published, no workflow was run, and no push was made.

## Public release verification

Pending publication.
