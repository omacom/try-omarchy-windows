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
| Native drag gesture on final candidate | Dragged a Unicode-named file from a native OLE source into the running moved guest; the SDL event and delivered Downloads contents matched. |
| Cancellation | Cancelled a 512 MiB transfer using the native Cancel control. No destination or partial file remained in Downloads; the next small drop succeeded. |
| Stable update mechanics | Signed test-key metadata for v1.0.0 and v1.0.1, native executable replacement/restart, retained previous executable and rollback passed three repetitions each. Preferences were unchanged. This does not establish production-key public stable-feed acceptance or a real stable guest boot. |
| Automated migration checks | The locked guest contract suite completed 157 tests with one skipped, including restore preservation and incomplete-restore reporting. Actual package/theme restoration on physical native Omarchy remains open. |

Large-drop SHA-256:
`e1f7ca7882b27e4a25e635f26c0d4e8788b17bbeec02c0a2c44d77189765cd34`.
Small-drop preservation SHA-256:
`1ae225002124966f47a5dbcb06818a71822c1154e12ed55f42bcfcafb8ab968e`.

Linux race tests, Linux vet, Windows cross-build and Windows vet with the project's
`-unsafeptr=false` setting passed. The final native Windows suite passed 347
tests with 32 environment-dependent skips. Source-reading tests were supplied
the repository fixtures. The new stable-transition tests also passed natively. CI run
[35476891886](https://github.com/omacom/try-omarchy-windows/actions/runs/35476891886)
passed Launcher, Windows launcher and Guest contract on source commit `904ace6`.
The full guest image job was not needed for this launcher-only change.
An initial harness race attempted rollback before its helper process exited;
the corrected harness waits for that PID, as the production update flow does.

## Full installation recovery

The production move functions copied and verified all 296 inventoried files,
including the 32 GiB disk, in 1,069 seconds on the external drive. A real Windows
sharing lock prevented moving the locked disk. The copy booted on polish7 and
preserved the 128 MiB fixture hash. Move redirect state used an isolated store
and a no-op shortcut callback; this did not exercise live Start menu shortcut
rewiring. The original installation remains intact.

A full snapshot was created on the copy. Injected zero free space refused reset
without replacing the active disk. A real reset then published a fresh disk and
retained the prior disk with unchanged SHA-256
`668cf77eefb60229ec07b2a6535e3fbd0725dcc9599f2c359b712c600da436c3`.
The snapshot/reset phase passed in 791 seconds. The fresh guest booted to Hyprland,
retained the 32 GiB capacity, had none of the old Downloads fixtures and reported
no failed user services. First-run provisioning on the external drive took
several minutes while the retained-disk checksum was also reading that drive;
this is not a normal-startup benchmark. Guest userspace readiness preceded
desktop and SSH readiness.

Snapshot rollback retained the fresh reset guest and restored the original disk.
Compaction then preserved the exact pre-reset SHA-256 above. The rollback and
compaction phase passed in 721 seconds. The reported 27,072,135,168 bytes of zero
ranges includes existing holes and is not a measurement of newly reclaimed space.
The recovered guest booted with both the 128 MiB fixture and Unicode drag file
unchanged, no failed user services, working camera capture and the saved device
preferences intact.

The final launcher also completed the real `-reclaim` flow: the guest acknowledged
an 8,192 MiB preparation pass, powered off cleanly, and the launcher compacted
the disk. Windows reported 8.0 GiB less allocated space compared with the prepared
disk immediately before compaction. This is not a claim of 8.0 GiB net savings
relative to the start of the entire test, since preparation itself writes zeros.
A final boot after reclaim preserved all eight Downloads fixture hashes, the
32 GiB capacity and a working desktop/clipboard bridge, with no failed user
services. Camera, microphone and automatic updates were enabled at handoff. All original
installations and recovery archives are retained.

The optional tiny-QEMU native-drop fixture did not open its QMP listener on this
laptop. A separate harness attached to the full running guest instead and passed
the actual OLE drag and delivery check. Three native transfer-window tests passed,
including Cancel and closing progress without cancelling the copy. The tiny
fixture startup failure remains unresolved; it is not counted as a passing test.

## Remaining release boundaries

- Public production-key preview-to-stable and stable-to-stable download chains
  cannot be claimed from test-key fixtures. Select the bridge feed and verify the
  published artifacts through the release process.
- Intel, NVIDIA, Core Ultra, Windows 10, mixed-DPI and sleep/wake coverage remain
  unverified on this AMD-only test setup.
- Native physical migration and a second-PC portable lifecycle remain unverified.
- Power loss during package writes remains distinct from the accepted orphaned
  package-lock recovery and controlled launcher rollback checks.
