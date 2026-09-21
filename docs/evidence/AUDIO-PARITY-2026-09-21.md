# Startup audio selection: September 21, 2026

Unpublished engineering candidate, tested on the existing AMD Ryzen 5 5625U /
Radeon Windows 11 laptop (build 26200). The user requested using the built-in
speaker and microphone; no second physical endpoint was available for this pass.
This extends [the launcher acceptance](MAC-PARITY-2026-09-21.md).

## Exact artifacts

| Artifact | SHA-256 |
| --- | --- |
| `TryOmarchy-audio4.exe` | `5781d3e3e1837dc7aa5c478c539e7695c840444f08c1c49569a602bb0b0bede1` |
| `TryOmarchy-audio4-tests.exe` | `bbab520686f81c8092e05ef87ca887d6897c17e95efa6be910c426ff89bc3f74` |
| r16 engineering `qemu-system-x86_64w.exe` | `159473348c4933a08fc656e856064a77f3225687f4e19075737430e2364efb15` |

Windows artifacts and logs are under `D:\TryOmarchy-Parity-20260921`.
The runtime is `audio-runtime`, selected explicitly with `-winq`. It was built
with isolated MSYS2 UCRT64 tools on D:, from the pinned QEMU/virglrenderer
commits and patches in the r16 source lock. Ten source regression scripts and
the native renderer shared-handle test passed. The candidate retains firmware
from the existing runtime; it is not a completed reproducible release archive.
The public runtime and `guest-build/runtime.lock.json` remain unchanged.

## Passed checks

- Linux `go test -race ./...`, `go vet ./...`, Windows cross-build, Windows vet
  with the existing unsafe-pointer analyzer exceptions, and `git diff --check`.
- Final interactive Windows suite: **361 top-level tests passed, 34 skipped,
  zero failures**. Skips are not acceptance of their optional/hardware paths.
- Native device enumeration; old-runtime disabled selectors; supporting-runtime
  independent selections, save/reopen persistence and return to defaults.
  Keyboard navigation includes both new selectors.
- Shortcut retry regression: five tests repeated ten times, all passing.
  An actual Windows sharing lock is released during retry; ownership is checked
  again before mutation. A changed owner is preserved and unrelated errors do
  not trigger retry.
- Diskless native runtime smoke: selected built-ins, nonexistent output fallback,
  nonexistent input fallback and microphone disabled. Expected directional
  warnings were observed; defaults opened successfully.
- Both QEMU launchers and qemu-img accepted Unicode paths. Saved RAM streamed
  completely, the source exited, and the restored VM resumed with its RAM intact.
- Existing guest booted twice with WHPX, virgl/Venus and the r16 runtime, once
  with microphone enabled and once disabled. Both shut down cleanly.
- Selected `Speaker (Realtek(R) Audio)` and `Microphone Array (AMD Audio Device)`
  using the native pre-boot controls. During a short generated playback test,
  the read-only Core Audio probe reported QEMU's active speaker session.
- With microphone enabled, guest PipeWire delivered **480,000 bytes** of mono
  48 kHz, signed 16-bit capture (five seconds). Samples were discarded immediately;
  no microphone recording was retained. The host probe reported the active AMD
  microphone endpoint belonging to the tested QEMU process.
- With microphone disabled through the native checkbox, QEMU used `in.voices=0`.
  The host probe reported only the speaker session during playback and attempted
  recording. The guest capture received **zero bytes** during a ten-second bound.
  Playback completed. The virtio input-backend warning in this mode is expected.
- An intentionally invalid inherited `SDL_AUDIO_DEVICE_NAME` did not interfere:
  the launcher removed it and applied the independent selections.
- The ordinary guest user executed the minimal nested-KVM vCPU probe on both
  boots: API 12, `KVM_EXIT_HLT` (exit reason 5), kernel `7.2.6-arch2-1`.

Endpoint session state demonstrates routing, not that a human heard the speaker
or that the physical microphone produced intelligible sound. Switching between
two physical outputs or inputs remains untested. This feature applies at startup;
stable endpoint IDs, live guest-driven routing and hot-unplug recovery are not
implemented by this change.

## Intermediate failures and corrections

The initial native smoke harness used redirected stdio QMP, which did not deliver
commands reliably on Windows. It now uses the existing bounded socket transport;
the final smoke passed. The invalid-UTF-8 fixture initially relied on Windows
GLib accepting an invalid environment value; injecting the getter now tests the
actual route function on both platforms.

An earlier full native suite hit a shortcut sharing violation. The final launcher
retries only sharing/locking errors with a bounded wait and checks ownership
again before each mutation. The deterministic lock tests, ten repeated runs and
the complete final suite passed. The process holding the original lock was not
identified.

## Final state and remaining work

The guest is powered off; no candidate launcher or QEMU remains running.
Microphone access was restored to its original enabled setting, and output/input
were reset to Windows default. Other desktop preferences, the existing guest,
public runtime, old candidates and recovery data were preserved. The artificial
SDL environment override was removed from the launch script.

The on-demand `TryOmarchy-Parity-AudioLaunch-20260921` task opens the tested
candidate against the existing guest with the explicit engineering runtime.
It has no automatic trigger. Private logs and candidate binaries are also retained
under `~/Documents/Codex/2026-09-21/try-omarchy-audio/` on the control machine.
Nothing was committed, pushed or published.

The laptop can register a Precision Touchpad window, but a complete pinch bridge
still needs implementation and gesture acceptance. Windows Hello availability
returned `DeviceNotPresent` in the signed-in session. No PAM or authentication
changes were made. Bridged networking remains separate driver/adapter work.
See [the parity tracker](../MAC-PARITY.md) for these boundaries.
