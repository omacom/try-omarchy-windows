# Repeatable Windows release acceptance

The routine below is intended to take about 15–25 minutes on prepared local
storage, plus a few minutes of hands-on use. This is a target to measure, not a
promised duration. Building, signing, downloading and preparing a baseline copy
are separate. The laptop's removable D: storage has previously taken 12–18
minutes for a single large copy, snapshot or move.

Use the same verified candidate throughout. Record source commit, launcher
SHA256 and Authenticode result, runtime archive SHA256, guest artifact hashes,
Windows build, rendering path, baseline version and elapsed times. An engineering
build or a directly patched guest can find bugs but does not accept a signed
release. Downloads are not a count of unique active installations.

## Every release

1. Complete the required CI and artifact checks in [RELEASING.md](RELEASING.md).
   Prepare one stopped copy of a known released installation, with baseline
   payloads retained for rollback, and one fresh trial directory. Keep them on
   local storage when space permits. Never reuse the owner's real install.
2. Boot the baseline copy and seed identifiable data with the guest check below.
   Upgrade that copy to the exact candidate using the supported pinned candidate
   flow. Check a visible, usable desktop, then verify the seeded files, empty
   directory, settings and account identity. Check the fresh candidate too.
3. Run the focused native Windows tests below on the interactive scheduled-task
   desktop. Read their results: a skipped test or a scheduled task returning zero
   does not prove a feature worked.
4. Perform a short hands-on pass: type into an app, Alt+Tab out and back, scroll
   vertically and horizontally, paste Unicode text in both directions, play a
   familiar clip, and record/play back a short microphone sample. Resize the VM
   and confirm the desktop remains usable. Record actual observations.
5. Cleanly shut down and relaunch the upgraded copy. Repeat the guest check and
   inspect new launcher logs for failed repair, rollback, repeated payload
   downloads or errors. Verify the expected desktop and seeded data again.
6. Test one forced pre-readiness update rollback on the disposable copy, then
   boot the previous payloads and compare the seed report. Accept only if the
   previous launcher/payloads and readable data return. Native test-key fixtures
   do not replace this exact-candidate check. For publication, also verify the
   public URLs and both signed feeds using the existing release procedure.

If a check fails, retain evidence, fix the cause and repeat the affected checks
on the corrected candidate. Do not publish a known regression or record a
timeout, skip or incomplete test as a pass. One AMD laptop cannot establish
Intel/NVIDIA, other Windows versions, mixed-DPI or Modern Standby coverage.

## Guest preservation and health check

`scripts/release/check-laptop-guest.py` runs against an already running guest
through a pinned SSH tunnel. It does not launch, update, stop or repair the VM.
`seed` writes only its uniquely named test data under the guest user's home;
`verify` reads it and compares with the saved baseline. Use one run ID throughout
the release round and a new output file for every phase. Keep reports outside the
public repository.

```bash
python3 scripts/release/check-laptop-guest.py seed \
  --host omarchy@127.0.0.1 --port GUEST_PORT \
  --key PRIVATE_KEY_PATH --known-hosts PINNED_GUEST_HOSTS \
  --run-id RELEASE_RUN --output PRIVATE_EVIDENCE/baseline.json \
  --preserve .config/hypr/input.lua --preserve .config/fcitx5/profile

python3 scripts/release/check-laptop-guest.py verify \
  --host omarchy@127.0.0.1 --port GUEST_PORT \
  --key PRIVATE_KEY_PATH --known-hosts PINNED_GUEST_HOSTS \
  --run-id RELEASE_RUN --baseline PRIVATE_EVIDENCE/baseline.json \
  --output PRIVATE_EVIDENCE/upgraded.json \
  --expect-compat REVISION:KERNEL_RELEASE --expect-module MODULE_VERSION \
  --expect-audio-sha256 AUDIO_SCRIPT_SHA256 --expect-unity
```

Omit `--expect-unity` if the test deliberately changes the hidden transport gain.
The check does not change gain or mute. It verifies a compositor output, running
bridges, playable Windows output routes, HTTPS access, expected versions, free
space and failed user units. The compatibility pin also checks the running
kernel; the battery version comes from the loaded module's sysfs entry.
It cannot prove pixels were drawn correctly,
microphone samples reached Windows, audible loudness or physical input feel.
After rollback, pin the expected *baseline* versions instead of candidate ones.
Seed before the update, not afterwards; otherwise preservation is not tested.
`--preserve` hashes existing real user settings without copying their contents
into the report. Choose files the update is meant to preserve. Their paths are
saved in the baseline and reused automatically during verification.

## Focused native Windows checks

Build the Windows test executable from the candidate's Go source. Set the QEMU
path to the candidate runtime. Run through an Interactive scheduled task, as
described in [REMOTE-LAPTOP-TESTING.md](REMOTE-LAPTOP-TESTING.md).

```powershell
$env:TRYOMARCHY_AUDIO_TEST_QEMU = 'C:\candidate\runtime\bin\qemu-system-x86_64w.exe'
& 'C:\candidate\native-tests.exe' '-test.v' '-test.timeout=120s' `
  '-test.run=^(TestNativeSignedStableUpdateAndRollback|TestPortableLauncherReplacementUsesBundleRoot|TestNativeAudioDeviceEnumeration|TestNativeStableAudioEndpointEnumeration|Test.*Battery.*|TestPayload.*|TestDirectoryUpdate.*)$'
if ($LASTEXITCODE -ne 0) { throw 'Native acceptance tests failed' }
```

These verify test-key signed updater replacement/rollback, preferences and native
endpoint/battery access. They do not use the public signing key or prove audible
playback. Save the full test log, including skips. The older
`scripts/vmtest/run-candidate.ps1` is a launch/log helper; its successful return
alone is not an acceptance verdict.

## Deeper checks when the affected code changes

| Change | Additional evidence |
| --- | --- |
| Guest image, packages or compatibility | `smoke-guest-upgrade.py` on devbox: baseline, upgrade, reboot, old-image boot and return to candidate; package-recovery smoke for package-update changes. |
| Updater, signing or bootstrap | Exact signed candidate and supported old-version paths, interruption before readiness, launcher/runtime/guest rollback, both feed signatures and public URL verification. |
| Audio, SDL or runtime | Actual 44.1/48 kHz formats, device switching/hotplug, mute/gain preservation, same-clip loudness and microphone off/on. |
| Input or display | Touchpad directions, both natural-scroll settings, wheel mouse, pinch then scroll, fullscreen, focus and relevant DPI/layout cases. |
| Clipboard or transfers | Native Win32 clipboard tests and real guest transfer round trip, Unicode/empty directories, hashes and original-file preservation. |
| Storage or recovery | Physical backup/restore, snapshot/rollback, reset, move, grow/reclaim and low-space cases that exercise the changed paths. |
| Battery or power | Compare Windows/UPower values, AC change and sleep/resume. Modern Standby and no-battery hosts need their own hardware coverage. |

Retain baseline assets and reusable scripts so each release does not recreate
the environment. Do not repeat full moves, long soaks and every hardware case
for an unrelated wording change; retain the last verified coverage and its date.
