# v0.4.0 signed candidate and release acceptance - September 26, 2026

v0.4.0 turns pinch to zoom on by default, adds Ctrl+Alt+End, and ships a guest
image built with patches through `0095` (lock refreshes `0092` and `0094`,
pinch rules for existing guests `0093`, pinch device declaration `0095`).
The runtime is unchanged: r20c, as in v0.3.0.

## Candidate

- Draft built by [prepare run 36226773027](https://github.com/omacom/try-omarchy-windows/actions/runs/36226773027).
  `SHA256SUMS` digest `f90e94a2b325e9048beb53459bf028cc008b5893c22787c0cbf406a54bb8cc02`.
  Its `build-spec.json` lists `virtio-pinch-pci` under `runtime.optionalDevices`.
- Pin pushed to master as `e242d55`. The [signing-check run 36227414802](https://github.com/omacom/try-omarchy-windows/actions/runs/36227414802)
  produced the tested launcher, SHA-256
  `08bdd956467edd6c9f5a16e0337a56d224d7087055f7019faf9f35ec3c4305e0`.
  On the laptop, Authenticode was `Valid`, signer `CN=Brandon South`, and
  FileVersion and ProductVersion were `v0.4.0`. Every uploaded asset matched
  `SHA256SUMS`.
- Assets were served over loopback on the laptop from `127.0.0.1:18080`. Every
  launch pinned both `-release` and `-runtime-release` to that server.

All candidate runs were on the AMD Windows 11 laptop under WHPX with GPU
rendering. The owner's real installation was not started or changed.

## Upgrade from v0.1.0 (copy 1)

Copy 1 was a Robocopy of the clean v0.1.0 install at
`D:\TryOmarchy-v0.1.0-20260922\clean` (223 files, 34,185,909,824 bytes on both
sides). It had never answered the shared-folder question, so the launcher asked
it; the test answered "Not now".

- The launcher logged `touchpad pinch forwarding: true (guest declares device: true)`
  without any pinch flag, and `guest update v0.4.0 confirmed after userspace reported ready`.
  Guest and runtime receipts recorded the v0.4.0 digest; the QEMU SHA-256 was the
  r20c value `44e6e56f88fcac5567bfc3995aa5113135efe18b22cd2d0316eb30819c254447`.
- Guest: kernel `7.2.7-arch1-1`, compat revision 34, `running` with 0 failed
  units. The existing disk keeps its own userspace (systemd 261.3).
- `try-omarchy-pinch-ready` reported `pinch gestures are ready`; the udev
  properties of the pinch device had no `LIBINPUT_IGNORE_DEVICE`. Catch-up
  appended the guarded loader to `input.lua` once and kept
  `input.lua.before-try-omarchy-pinch`. `hyprctl configerrors` was empty and the
  device list included `qemu-virtio-pinch-touchpad`.
- The preserved marker `~/Documents/v010-clean-marker.txt` kept SHA-256
  `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`.
- A guest reboot relaunched through the supervisor with pinch still ready, and
  catch-up did not repeat. After a clean poweroff, a second launch requested only
  `SHA256SUMS` from the payload server.

## Fresh install

A new data directory with `-instant` downloaded the v0.4.0 payload and reached
userspace 42 seconds after boot started. The test answered "Not now" to the
shared folder and deselected both shortcut choices; no shortcut was created.

- systemd `262-1`, hyprland `0.56.2-3`, hyprtoolkit `0.6.0-1`, chromium
  `153.0.8010.52-1`, kernel `7.2.7-arch1-1`; `running` with 0 failed units; root
  filesystem 24 GiB.
- Pinch ready, one loader block in the new user's `input.lua`, no Hyprland
  config errors, desktop background and bar mapped, not locked. The Windows audio
  endpoints appeared as PipeWire sinks.
- A guest reboot relaunched with pinch forwarding on and userspace ready.

## Pinch and scrolling

Physical fingers were not used on this exact candidate. The owner's physical
pinch and Ctrl+Alt+End checks passed earlier on September 26 on the guest
migrated by `0093`, through the #183 launcher with `-experimental-pinch`.

On this candidate, `runtime-build/inject-pinch-test.c` (source hash matching the
repository) injected two-contact touchpad input through Windows into the
foreground QEMU window, and `scripts/guest/check-pinch-events.py` observed the
guest's libinput:

| Gesture | Copy 1 | Fresh install |
|---|---|---|
| Out | begin, 42 updates, end; max scale 1.661 | begin, 42 updates, end |
| In | begin, 38 updates, end; min scale 0.552 | begin, 37 updates, end |
| Cancel | begin, 9 updates, end | not run |
| Focus loss | begin, 11 updates, end | not run |
| Pan | tablet scroll only, no pinch | tablet scroll only, no pinch |

No run produced a button event or a libinput error.

Native Wayland Chromium with a temporary profile measured
`visualViewport.scale` over DevTools. On both copy 1 and the fresh install it
went from 1.00 to 1.66 after pinching out and back to 1.00 after pinching in.
The page counted ctrl+wheel pinch events and zero `pointerdown` events. A
two-finger pan reached the page as plain wheel events at scale 1.00.

Two test obstacles were cleared without changing the product: the CSSI reboot
popup covered the window center, so it was moved to the top-left corner (no
button was pressed), and copy 1's idle lock had engaged after five minutes, so it
was unlocked with the trial password and kept awake only for the test.

## Rollback (copy 2)

Copy 2 was a second copy of the same v0.1.0 source. The candidate began its
first v0.4.0 boot at 10:57:45 and the test stopped the launcher and QEMU at
10:57:47, before userspace was ready. `payload-update-state.json` recorded guest
and runtime pending, with `guest.previous` and `runtime.previous` retained.

The next launch logged `using restored guest and runtime for this recovery launch`,
requested nothing from the payload server, and reached userspace. Both receipts
were back on the v0.1.0 digest
`3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`. The
launcher logged `touchpad pinch forwarding: false (guest declares device: false)`,
and the restored guest had no pinch input device. It ran kernel `7.2.6-arch2-1`
with 0 failed units and the unchanged marker hash, then powered off cleanly.

## Publication

[Publish run 36259677015](https://github.com/omacom/try-omarchy-windows/actions/runs/36259677015)
signed, published and verified the release, then marked v0.4.0 Latest. The
public launcher SHA-256 is
`de64d114c6ab23a7ee9de75c16155894f89e93960df8f422e31415948fa94f24`; it is a
separate signed build of the same commit as the tested candidate.
`releases/latest/download/TryOmarchy.exe` and the tryomarchy.com download
routes resolve to v0.4.0. The master CI pin check passed after publication.

## Not covered

Physical fingers on this exact build, Firefox, fullscreen and mixed-DPI pinch,
Windows 10, other touchpads, and the public update path from an installed v0.3.0
launcher.
