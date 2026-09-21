# September 21 parity handoff

## Source and integration state

Repository: `omacom/try-omarchy-windows`. Working branch:
`codex/laptop-control-handoff`. The parity implementation and this handoff are
committed together; use the branch HEAD as the source checkpoint. No release or
runtime archive was published, and the public release/runtime lock is unchanged.

The tested launcher/runtime were built before the source commit; their exact
hashes are in the evidence documents. Committing the source does not turn them
into signed release artifacts.

At handoff, this branch starts from `2978be6` (remote-control documentation) over
`b1d3b65`. Fetched `origin/master` is `3c0e532`, with six newer commits including
host-aware resource profiles, GPU/WSL tooling, and removal of the public v1 roadmap.
Those changes overlap `main.go`, Settings, backup/restore and documentation.
They were deliberately not merged during this handoff: preserve the physically
tested checkpoint, then integrate current master on a continuation branch and
rerun checks for the actual combined changes. Do not reintroduce the removed
public roadmap or overwrite the newer resource controls. Existing older docs
may refer to that roadmap until integration reconciles them.

## Implemented and accepted

- Native pre-boot launcher using the four Settings pages, explicit start/menu
  modes, About actions, visible recovery dialogs, location ownership and bounded
  shortcut-sharing retries.
- Branding/resource metadata and third-party notices, retaining original credit.
- Separate startup playback/recording choices, runtime capability gating,
  microphone-off behavior, fallback and backup/diagnostic integration.
- Experimental Windows Precision Touchpad bridge, dedicated virtual touchpad,
  guest configuration, lifecycle cancellation and per-SDL-poll motion coalescing.
- Nested KVM probes and a diskless real Linux kernel/PID 1 acceptance fixture.
- Guest patches `0083` and `0084`: seeded device configuration and an eight-package
  version refresh, with unchanged package membership/requested-input hashes.

Evidence:

- [Launcher and branding](evidence/MAC-PARITY-2026-09-21.md)
- [Audio](evidence/AUDIO-PARITY-2026-09-21.md)
- [Pinch, fresh image and nested Linux](evidence/PINCH-NESTED-2026-09-21.md)
- [Feature comparison](MAC-PARITY.md)

Windows launcher suite: 363 top-level passes, 34 skips, zero failures. Runtime
regressions passed on the final r17 binary, including audio, Unicode paths,
virtual touchpad PCI/virtqueue delivery and memory migration with that device.
Linux race tests and the guest contract passed (157 guest tests, one skip).
A full factory image built and booted separately on the physical Windows laptop.
Its first provisioning took about 4 minutes 46 seconds on the external drive;
there were no failed system services or Hyprland configuration errors afterward.
Native Wayland Chromium zoomed from 1.00 to 1.66 and back to 1.00 with synthetic
Windows touchpad input. Normal-user KVM and real nested Linux boot/poweroff/reboot
requests passed on the final runtime and fresh guest.

## Laptop, artifacts and cleanup

Read [REMOTE-LAPTOP-TESTING.md](REMOTE-LAPTOP-TESTING.md), then the machine-local
`~/.local/share/try-omarchy-laptop-control/README.md`. It contains the exact pinned
SSH connection, guest helpers, tasks, paths and candidate hashes. Recheck that the
laptop is online and signed in; do not assume yesterday's connection state.
The user authorized remote testing and requested continuing without unnecessary
questions. Use built-in speaker/microphone; do not ask for a headset again.

Both guests shut down cleanly. No candidate launcher or QEMU process remained.
All retained pinch tasks are on-demand with no automatic triggers. The temporary
factory and browser forwarding connections were closed; the original remote
control setup was preserved. The test browser service was stopped. Audio choices
are Windows defaults, and the existing microphone setting remains enabled.
The existing guest retains only the documented device-specific pinch override,
with its pre-change `input.lua` backup. Its original data and recovery copies,
earlier candidates and public runtime are preserved.

Intentionally retained, outside Git:

- Windows `D:\TryOmarchy-Parity-20260921`: named launcher candidates, `pinch-runtime`,
  isolated `pinch-factory`, build sources and on-demand acceptance scripts/tasks.
- `~/Documents/Codex/2026-09-21/try-omarchy-{parity,audio,pinch}/`: private evidence,
  candidate binaries and source snapshots. Do not publish raw screenshots or
  machine-specific logs without review.
- `/data/try-omarchy-pinch-build-20260921/`: full factory artifacts and isolated
  Docker build cache (`try-omarchy-pinch-work-20260921`), and retained QEMU/virgl
  scratch checkouts (the old `/tmp` names are symlinks). These save a full rebuild;
  do not delete them as generic temporary-file cleanup.
- Local scratch guest/runtime checkouts under `/tmp` are conveniences, not the
  source of truth. Repository patch series and locks are authoritative.
- The pre-existing ignored root `TryOmarchy.exe` is an older build, not the current
  candidate. It was preserved rather than deleted or committed.

Generated Python bytecode caches were removed. The final Linux race-test rerun
initially exhausted the `/tmp` quota during large file-transfer tests; use a
private `TMPDIR` on `/data` for those tests rather than filling the memory-backed
scratch filesystem. The complete race suite passed with that disk-backed directory. No credentials, downloaded runtime
binaries, factory images or private evidence are included in the source commit.
The checked-in Windows `.syso` resource is intentionally regenerated source output.

## Remaining work and guardrails

1. Integrate current master and preserve both resource-profile and parity behavior.
   Keep the existing candidate/evidence as a rollback reference; use a fresh
   filename for any rebuilt Windows candidate.
2. Pinch remains opt-in (`-experimental-pinch`, supporting r17 runtime, GPU,
   one display). Physical fingers, subjective smoothness, Firefox, fullscreen,
   mixed DPI, Windows 10 and other touchpads remain untested. Synthetic acceptance
   is not evidence of all those behaviors. Do not silently enable it by default.
3. Audio selection is startup-only. Live routing and stable endpoint IDs remain
   feature work; switching between two physical endpoints remains untested.
4. Windows Hello sudo is unimplemented. This laptop reported `DeviceNotPresent`;
   retain password authentication and do not substitute an unverified PAM bridge.
5. Bridged networking is unimplemented. NAT/explicit forwarding work. No TAP,
   Wintun, Hyper-V or bridge adapter matched the host inventory; do not silently
   install drivers or reconfigure the user's host networking.
6. Reconcile release packaging/provenance and final-candidate acceptance before
   proposing a release. Committing/pushing this branch does not publish anything.

Continue useful implementation and tests autonomously. Preserve guest data,
recovery copies and credential pins. Ask only when genuinely blocked by missing
hardware, a consequential product decision or an action outside authorization;
state the specific blocker rather than repeatedly asking to continue.
