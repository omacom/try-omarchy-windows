# Windows audio device choices

`v0.1.0` introduced **Sound output** and **Microphone** selectors to
**Devices**. Each direction can use **Windows default** or a device enumerated
by the selected runtime's SDL library. The `v0.2.0` release applies choices at
VM startup. The public `v0.3.0` release adds live switching with r20c: host
Settings and Omarchy's guest audio switcher can change playback and recording
choices while the VM runs, and selected devices persist across guest reboots.
The microphone access gate still applies at the next VM start.

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
  until changed. On the older r19 runtime, hot-unplug and default-device changes
  during playback still depend on SDL/Windows; restart the VM if routing is not
  recovered. r20c polls live routes and falls back if a selected endpoint
  disappears. Physical hotplug acceptance remains untested.
- Preferences retain SDL device names and, when a name uniquely matches an active
  Core Audio endpoint, its stable Windows endpoint ID. The ID resolves the current
  friendly name at each start, so ordinary renames and reboots do not discard the
  selection. Ambiguous duplicate names remain name-based. Live guest-driven
  switching ships in `v0.3.0` with r20c; public `v0.2.0` remains a previous
  startup-only release. This is not full Mac audio parity.

`audio-preferences.json` and the separate `audio-endpoints.json` live beside
`settings.json` and are included in current backups and recovery copies. Keeping
the IDs separate lets older launchers continue reading the name preferences during
rollback. Older launchers ignore the ID file. Device enumeration does not open
playback or recording streams. Diagnostic bundles report whether a selection
exists and omit device names and endpoint IDs.

## r20c live-route support

The r20 source recipe adds a private `vm/audio-control` directory. When the
runtime contains `0016-live-sdl-audio-routes.patch`, the launcher writes separate
output and input routes before QEMU starts. Saving audio choices in Settings
writes atomically replaced route files; the active SDL backend polls them and
reopens a changed route without restarting the guest. The microphone permission
gate still requires a new VM start because QEMU creates its input voices at
launch. Unsupported runtimes retain the startup-only behavior above.

The r20c runtime shipped as `runtime-v1-r20c` in `v0.3.0`. Its loopback-only
virtio serial catalog and guest PipeWire service offer active Windows endpoints
that SDL can identify unambiguously. Choosing one in Omarchy saves its stable
endpoint ID and updates QEMU's live route; Settings choices are polled back into
the guest. Microphone-off removes host input choices from the guest picker.
The [signed and public acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md)
passed playback, capture, live route changes from the guest and host Settings,
saved-choice persistence across guest restart, idle release, rollback, and the
public update path. The [signed packaged candidate checks](evidence/LIVE-AUDIO-R20-2026-09-23.md)
also passed the rebuilt image's playback, capture, route fallback, microphone
permission, idle release, and Windows boot checks. Public `v0.2.0` used r19 and
startup-only choices. Two physical endpoints per direction and hotplug remain
untested.

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
