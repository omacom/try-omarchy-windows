# Desktop controls and recovery verification, September 19, 2026

This is development-candidate evidence, not a published release. Public Latest
remains v0.0.20-preview. The test host is the same physical AMD/Radeon Windows 11
laptop used for the [v1 acceptance pass](V1-LAPTOP-2026-09-19.md). No additional
hardware or separate native Omarchy installation was available. Sleep/wake was
not tested.

## Candidate and scope

The candidate adds four Settings pages, memory in GB, camera selection and access,
microphone access, automatic-update preferences, About/manual update checks and
camera status. It also prevents a failed file drop from blocking later drops
behind an error dialog, and fixes Tab navigation out of the port-forward editor.

Development launcher `TryOmarchy-polish7.exe` is unsigned, built from this branch.
SHA-256: `d800c0de8191be385fdf884c3462d60daded7faf77de651694eeebfb97a72838`.
The runtime and guest remain the accepted public v20 payloads: guest compatibility
29, kernel 7.2.6, runtime archive SHA-256
`8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc`.
Device checks used earlier development builds with the same device implementation;
file-drop recovery used polish5, and the final keyboard correction used polish7.
A signed release still requires validation of its exact published artifacts.

## Completed checks

| Check | Result |
| --- | --- |
| Camera disabled, then enabled with the built-in camera selected | Disabled requests returned the Settings explanation without activating Windows capture. Enabled capture produced 30 valid, distinct frames. |
| Missing selected camera | Native Media Foundation test returned the disconnected-camera error, with no fallback to the default camera. |
| Microphone disabled | Windows microphone usage timestamps did not change during a guest recording attempt; playback still worked. |
| Microphone enabled again | Guest recording produced 233,472 frames with nonzero audio data; Windows recorded fresh microphone use. |
| Settings | All four pages inspected on the laptop. Save/reopen preserved device choices. Thirty Tab presses per page stayed on visible controls; Advanced navigation now leaves the multiline port-forward field. |
| Manual update check | The native About dialog authenticated the official feed and reported installed/latest v0.0.20-preview without replacing the running installation. |
| Drop before guest readiness | Reproduced the hidden modal error blocking the transfer worker. After the fix, two early drops reported errors and a later drop succeeded without dismissing a dialog. |
| Large drop | A 128 MiB random file arrived in Downloads with an exact SHA-256 match. |
| Cancellation | Cancelled a 512 MiB transfer using the native Cancel control. No destination or partial file remained in Downloads; the next small drop succeeded. |
| Stable update mechanics | Signed test-key metadata for v1.0.0 and v1.0.1, native executable replacement/restart, retained previous executable and rollback passed three repetitions each. Preferences were unchanged. This does not establish production-key public stable-feed acceptance or a real stable guest boot. |
| Automated migration checks | The locked guest contract suite passed 157 tests, one skipped, including restore preservation and incomplete-restore reporting. Actual package/theme restoration on physical native Omarchy remains open. |

Large-drop SHA-256:
`e1f7ca7882b27e4a25e635f26c0d4e8788b17bbeec02c0a2c44d77189765cd34`.
Small-drop preservation SHA-256:
`1ae225002124966f47a5dbcb06818a71822c1154e12ed55f42bcfcafb8ab968e`.

Linux race tests, Linux vet, Windows cross-build and Windows vet with the project's
`-unsafeptr=false` setting passed. The initial native Windows suite passed 343
tests with 32 environment-dependent skips. Source-reading tests were supplied
the repository fixtures. The new stable-transition tests also passed natively.
An initial harness race attempted rollback before its helper process exited;
the corrected harness waits for that PID, as the production update flow does.

## Full installation recovery

A separate full installation copy is being exercised for move, reset, snapshot
rollback and compaction. The original installation and recovery archives are
retained. Completion and boot checks will be recorded here before this draft is
considered ready. A real Windows sharing lock was already confirmed to prevent
moving the locked disk.

## Remaining release boundaries

- Public production-key preview-to-stable and stable-to-stable download chains
  cannot be claimed from test-key fixtures. Select the bridge feed and verify the
  published artifacts through the release process.
- Intel, NVIDIA, Core Ultra, Windows 10, mixed-DPI and sleep/wake coverage remain
  unverified on this AMD-only test setup.
- Native physical migration and a second-PC portable lifecycle remain unverified.
- Power loss during package writes remains distinct from the accepted orphaned
  package-lock recovery and controlled launcher rollback checks.
