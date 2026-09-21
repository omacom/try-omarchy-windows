# Pinch and nested Linux continuation — September 21, 2026

Unpublished engineering work on the existing Ryzen 5 5625U / Radeon laptop,
Windows 11 build 26200. This extends [audio acceptance](AUDIO-PARITY-2026-09-21.md).
The user authorized continuing remote work without requiring their participation.

## Candidate identity

| Artifact | SHA-256 |
| --- | --- |
| `TryOmarchy-pinch1.exe` | `bfbee4fcb41b54246d7c9fb2d839a4ff29345dc1721e25785bbd476d8737e51b` |
| `TryOmarchy-pinch1-tests.exe` | `bc290a8784c38db8db38d0d9a491d01058df99379dfdea025b77e328744903d2` |
| r17 `qemu-system-x86_64w.exe` | `c8eb534adb05d8117e89413e696f3005769f763c028ac77436999f5439030d31` |
| Nested Linux kernel `7.2.6-arch2-1` | `a4671ce8cd648aaf5af83f9a65163c4d6c7083dd63099f16426bd1bd4f0c888f` |
| Nested QEMU `11.1.1` | `87f21559c0449a475f5c514289ac121bf934263cbf7d2dfbb5a1d998028bbd85` |
| Final diskless initramfs | `92b9758a2f38ba17fbffee741aff45ee3a39722d3df30bacab16f131ebb76e93` |

Windows candidates are under `D:\TryOmarchy-Parity-20260921`; the new runtime is
`pinch-runtime`. The previous `audio-runtime` remains intact. The r17 recipe adds
patch `0014`; the public runtime/release lock has not changed. This is an isolated
engineering build using the prior candidate's dependencies/firmware, not a signed
or fully packaged release.

## Pinch implementation and physical-host automation

- Added a dedicated virtual touchpad, dynamic Windows touchpad/Interaction Context
  API loading, native pan fallback, bounded diagonal pinch geometry, and contact
  cancellation on focus loss, VM state changes, reset and window destruction.
- Added `-experimental-pinch`. It requires supporting runtime provenance, GPU
  mode and one display. Normal launches do not register for touchpad messages.
- The actual Windows build compiled and linked both QEMU executables. Native
  qtest read the PCI capabilities, negotiated the virtqueue, observed contacts
  and releases, and verified ordinary buttons still target the tablet.
- The guest's Hyprland recognized `qemu-virtio-pinch-touchpad`. Its device-only
  no-tapping/no-disable-while-typing override passed reload/config-error checks.
  A pre-change `input.lua.before-pinch-20260921` backup was retained.
- Synthetic input used Microsoft's native touchpad injection API in the signed-in
  session, targeting the exact candidate PID and its foreground SDL window.
  The observer used the guest's actual libinput and opened no keyboard devices.
- Outward pinch: begin, 42 updates, end; maximum reported scale about **1.661**.
  Inward pinch: begin, 38 updates, end; minimum scale about **0.552**.
- Explicit cancellation and focus loss each produced a pinch end and no click
  events. Earlier builds logged transient libinput touch-jump warnings. Multiple
  Windows history callbacks delivered in one SDL poll appeared as implausibly
  fast motion. Coalescing motion once per poll while preserving contact
  transitions fixed the warnings. Final outward, inward, cancel, focus, pan and
  pause/resume tests passed with **no libinput errors**; the observer now rejects
  those errors explicitly.
- Two-finger pans produced tablet wheel events, with no pinch or click events,
  including after a clean guest reboot.
- Pausing an active gesture and resuming produced a pinch end with no clicks;
  QMP confirmed the VM returned to `running`. The initial pause timing preceded
  guest pinch recognition; the retained test pauses after recognized motion.
- The final runtime completed accelerated boot and clean poweroff. An earlier
  r17 build also passed a clean guest reboot through the launcher supervisor.

Physical fingers, Firefox, fullscreen/mixed-DPI behavior, Windows 10,
and other touchpads were not tested. Native Wayland Chromium zoom passed in the
fresh-image run below. Synthetic injection is useful integration
evidence but does not establish physical usability. The feature remains opt-in.

## Nested Linux acceptance

The ordinary Omarchy user booted a diskless Linux `7.2.6-arch2-1` kernel and static
PID 1 using QEMU 11.1.1, `q35,accel=kvm`, `-cpu host`, one CPU and 256 MiB RAM.
QMP reported both `present: true` and `enabled: true`; TCG fallback was forbidden.
Both poweroff and reboot-request runs reached the readiness marker and exited
with status zero, first on an earlier r17 build and then on the final runtime
with the fresh factory guest. The reboot run used `-no-reboot` to stop after that request.

QEMU and its dependencies were temporarily bundled from the control machine;
no guest package installation, disks or network devices were involved. The
standalone fixture builder and guest probe are retained in the repository.
The minimal normal-user KVM HLT probe also passed after the outer guest reboot.
This proves a real nested kernel/userspace boot, not a full distribution install,
network/storage workload or support across other host configurations.

## Source validation and intermediate corrections

Linux Go race tests and Linux/Windows vet checks passed. The compiled geometry
and native output-dispatch tests cover pure pans, invalid values, bounds,
cancellation and pause/resume. The guest recipe's contract check passed; its
suite ran **157 tests with one skip**. Patch `0083` supplies the device-specific
configuration to new factory users. Patch `0084` refreshes exactly eight package
versions required by the current repositories; package membership and requested
input hashes are unchanged. The complete factory image built successfully.

The unchanged launcher passed the native Windows suite: **363 passed, 34 skipped,
zero failures**. After the final timing-only runtime fix, the runtime suite passed
again: source regressions, real virtual touchpad PCI/virtqueue ABI, four SDL audio
cases, Unicode paths, and memory save/restore with the pinch device included.

Initial Windows ABI transport used AF_UNIX, unavailable in the installed native
Python; the harness now uses bounded loopback TCP. One new Windows API was only
exported by its documented ordinal, so named lookup now has that fallback.
A pure-pan callback was initially consumed as a zero-scale pinch; native dispatch
now waits for actual scaling, with a regression test and physical-host retest.
Horizontal synthetic contacts caused slow inward gestures to become scrolls in
libinput. Diagonal contacts fixed both-direction recognition. The virtual device
now advertises its clickpad property, avoiding the missing-right-button warning.

## Fresh factory image acceptance

The full container build produced these SHA-256 identities:

| Artifact | SHA-256 |
| --- | --- |
| `rootfs.ext4.zst` | `84f6601fa43dbf7a528890e8d3a96440ca92325556c84b96f7d442da31417513` |
| Unexpanded `rootfs.ext4` | `e64ebac21078cc66a42d29b58230b9fa4673b3214f32ce5d356b50437ed48f3e` |
| `initramfs-linux.img` | `3595309751b930b68e0d1524142cc2a3d0a1cc0a25abc403ce24d9ed9ef0c232` |

Kernel identity is in the candidate table. The compressed and raw disk hashes
were verified on Windows before expanding the disposable disk to 24 GiB. The
first manual decoder attempt used zstd's default window limit and failed without
producing disk contents; the retry enabled long-window decoding and verified the
raw hash. No launcher decoder change was needed.

An isolated direct-QEMU task booted this image on the final r17 runtime, with
GPU acceleration, explicit instant provisioning and SSH restricted to a host
loopback forward. It had no shared folder and did not register another launcher
installation. This validates the image/runtime, not a fresh launcher download or
interactive account-creation flow. The previous guest remained powered off.

First-boot userspace took about 4 minutes 46 seconds on the external test drive;
it eventually completed provisioning with zero failed system services. The root
filesystem expanded to 24 GiB. The new user's `input.lua` automatically loaded
`pinch-input.lua`, Hyprland reported no configuration errors, and its device
inventory included the dedicated touchpad. Outward/inward pinch, cancellation
and focus-loss tests passed without clicks or libinput errors.

A dedicated temporary Chromium profile ran a local data-URL page using native
Wayland (`xwayland: false`). CDP measured `visualViewport.scale` changing from
**1.00 to 1.66015625** after Windows touchpad injection, then back to **1.00**
after inward pinch. No keyboard zoom shortcuts or CDP input injection were used.
The browser test service was stopped afterward. Normal-user KVM HLT and both
nested Linux exit modes passed again on this fresh guest and final runtime.

Build output/cache remain under `/data/try-omarchy-pinch-build-20260921/`;
Windows retains the isolated `pinch-factory` test directory and on-demand task.

## Retained state and remaining gaps

Both test guests powered off cleanly. The existing guest disk, public runtime, earlier
candidates and recovery copies were retained. Audio remains on Windows defaults
with the original microphone setting. The guest's device-only pinch override and
its backup remain, so the explicit experimental candidate can be retested.

Private logs/binaries are retained under
`~/Documents/Codex/2026-09-21/try-omarchy-pinch/`; Windows has the on-demand
`TryOmarchy-Parity-PinchLaunch-20260921` and acceptance/injection tasks. None has
an automatic trigger. Review task actions before reuse; injection tasks move
the cursor and temporarily change focus. No release was published. The source is retained on
`codex/laptop-control-handoff`; see [the next-session handoff](../NEXT-SESSION.md).

Hello still reports `DeviceNotPresent`. No authentication/PAM changes were made.
No TAP, Wintun, Hyper-V or bridge adapter matched the read-only host inventory;
no driver or network settings were changed. Live audio routing and stable audio
endpoint IDs remain unimplemented; two physical device switching remains untested.
