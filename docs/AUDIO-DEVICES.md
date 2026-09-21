# Windows audio device choices

The candidate launcher adds **Sound output** and **Microphone** selectors to
**Devices**. Each direction can use **Windows default** or a device enumerated
by the selected runtime's SDL library. Choices apply when the VM next starts;
saving Settings while Omarchy runs does not switch an active stream.

This requires the unreleased r16 runtime recipe's
`0013-select-sdl-audio-devices.patch`. The published v20 runtime does not contain
this patch: its selectors stay disabled and Windows defaults remain in use.
The guest/runtime release lock remains unchanged until runtime acceptance and
release packaging are complete.

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
  until changed. Hot-unplug and default-device changes during playback still
  depend on SDL/Windows; restart the VM if routing is not recovered.
- Preferences identify SDL device names, including SDL's duplicate-name suffixes.
  Renaming a Windows device or reordering identically named devices may require
  selecting it again. Stable endpoint IDs and live guest-driven switching remain
  follow-up work; this is not full Mac audio parity.

`audio-preferences.json` lives beside `settings.json` and is included in current
backups and recovery copies. Keeping it separate lets older launchers continue
reading their existing preference files during rollback. Older launchers ignore
these selections. Device enumeration does not open playback or recording streams. Diagnostic
bundles report whether a selection exists and omit device names.

## Validation

The [September 21 physical acceptance](evidence/AUDIO-PARITY-2026-09-21.md)
passed built-in speaker/microphone routing, startup fallback and microphone-off
checks on the r16 engineering runtime. Two-device switching remains untested.

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
check enumeration, disabled choices with an older runtime, independent choices
with a supporting runtime, persistence on reopening, and return to defaults.
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
