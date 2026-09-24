# Windows audio device choices

`v0.1.0` adds **Sound output** and **Microphone** selectors to
**Devices**. Each direction can use **Windows default** or a device enumerated
by the selected runtime's SDL library. In public `v0.2.0`, choices apply when
the VM next starts; saving Settings while Omarchy runs does not switch an active
stream.

The bundled r19 runtime in `v0.2.0` includes
`0013-select-sdl-audio-devices.patch`, as did the prior r18 runtime. The older
v20 runtime lacks this patch; when selected instead, its selectors stay
disabled and Windows defaults remain in use. A separately managed external
runtime may also lack the patch.

## Behavior

- Output and input use separate choices. Changing Try Omarchy's selection does
  not change the Windows system default or another application's route.
- **Allow microphone access** remains the recording gate. Disabling it leaves
  playback enabled and prevents a saved microphone choice from enabling input.
- A selected device that cannot open at startup falls back to the Windows
  default for that direction, with a warning in `vm/qemu-stderr.log`.
- If neither the selection nor the default opens, the existing launcher audio
  fallback applies. A non-SDL or older runtime logs that it cannot apply saved
  selections and uses defaults.
- Reopen Settings to refresh the device list. A disconnected choice is retained
  until changed. On public `v0.2.0`, hot-unplug and default-device changes during
  playback still depend on SDL/Windows; restart the VM if routing is not
  recovered. r20c polls live routes and falls back if a selected endpoint
  disappears.
- Preferences retain SDL device names and, when a name uniquely matches an active
  Core Audio endpoint, its stable Windows endpoint ID. The ID resolves the current
  friendly name at each start, so ordinary renames and reboots do not discard the
  selection. Ambiguous duplicate names remain name-based. Live guest-driven
  switching is supported by r20c, pinned for the v0.3.0 candidate; public
  v0.2.0 remains startup-only. This is not full Mac audio parity.

`audio-preferences.json` and the separate `audio-endpoints.json` live beside
`settings.json` and are included in current backups and recovery copies. Keeping
the IDs separate lets older launchers continue reading the name preferences during
rollback. Older launchers ignore the ID file. Device enumeration does not open
playback or recording streams. Diagnostic bundles report whether a selection
exists and omit device names and endpoint IDs.

## r20c live-route candidate

The r20 source recipe adds a private `vm/audio-control` directory. When the
runtime contains `0016-live-sdl-audio-routes.patch`, the launcher writes separate
output and input routes before QEMU starts. Saving audio choices in Settings
writes atomically replaced route files; the active SDL backend polls them and
reopens a changed route without restarting the guest. The microphone permission
gate still requires a new VM start because QEMU creates its input voices at
launch. Unsupported runtimes retain the startup-only behavior above.

The r20c candidate includes a loopback-only virtio serial catalog and a guest
PipeWire service. It offers active Windows endpoints that SDL can identify
unambiguously, and choosing one in Omarchy saves its stable endpoint ID and
updates QEMU's live route. Settings choices are polled back into the guest.
Microphone-off removes host input choices from the guest picker. The r20c
runtime is published as `runtime-v1-r20c` and pinned for the v0.3.0 candidate.
The [signed packaged candidate checks](evidence/LIVE-AUDIO-R20-2026-09-23.md)
passed playback, capture, route fallback, microphone permission, saved-choice
persistence across guest restart, idle release, and Windows boot with the
rebuilt image. The signed laptop candidate changed real speaker and microphone
routes through the guest picker and survived rapid two-direction changes. A
disposable Linux upgrade and boot smoke also passed for the rebuilt image.
Public `v0.2.0` remains on r19 until v0.3.0 publishes. Two physical endpoints
per direction and hotplug remain untested.

## Validation

The [September 21 physical acceptance](evidence/AUDIO-PARITY-2026-09-21.md)
passed built-in speaker/microphone routing, startup fallback and microphone-off
checks on the r16 engineering runtime. The [September 22 integration pass](evidence/PARITY-MASTER-INTEGRATION-2026-09-22.md)
passed stable endpoint enumeration and persistence on the same laptop. A physical
rename and two-device switching remain untested.

Local regression coverage includes preference round-trip/corruption, backup
round-trip, direction separation, microphone disablement, inherited environment
cleanup and old-runtime capability gating. The runtime build compiles the exact
patched route function against a fake SDL device opener to cover independent
routes, failed selections, failed defaults and invalid UTF-8.

Native Windows tests use:

```powershell
$env:TRYOMARCHY_UI_TEST='1'
$env:TRYOMARCHY_LAUNCHER_TEST_EXE='<candidate launcher>'
$env:TRYOMARCHY_AUDIO_TEST_QEMU='<runtime>\bin\qemu-system-x86_64w.exe'
& '<candidate tests>' -test.v '-test.run=TestNativeAudioDeviceEnumeration|TestAudio'
```

Run these on the signed-in desktop with no other launcher window. Native tests
check SDL and stable endpoint enumeration, disabled choices with an older runtime,
independent choices with a supporting runtime, persistence on reopening, and
return to defaults.
The settings test uses a temporary installation and does not boot a VM.

Hardware acceptance for each release candidate must additionally boot the existing guest with the rebuilt
runtime, play a short test sound, exercise recording with microphone permission
off/on, and test a nonexistent device's startup fallback. Two physical endpoints
are needed to prove routing away from the system default. Track those results
separately from source-level tests and successful compilation.

`runtime-build/probe-audio-sessions.c` is a read-only acceptance helper: while
the guest plays or records, it reports Windows endpoint sessions belonging to a
specified QEMU process. It uses Microsoft's documented
[session enumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)
and [process identity](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid)
APIs. A reported active session is routing evidence, not proof that a human heard
the speaker or that a physical microphone produced intelligible audio.
