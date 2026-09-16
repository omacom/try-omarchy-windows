# Windows laptop acceptance, September 13, 2026

Status: in progress. This is physical Windows acceptance of PR #110, not a
release approval. The user selected the current launcher with runtime r7 and
the corrected compatibility-19 guest-r2. Existing native Omarchy testing is
outside this run's scope.

Latest checkpoint: storage recovery and retained-source cleanup pass; runtime
r12 passed three-output GPU/CPU DPMS checks and its one-hour endurance run.
Shutdown confirmation and forced GPU-to-CPU fallback also pass. Full snapshot
creation and restore-as-copy now pass, including whole-disk hash verification
and a real desktop boot. Active rollback also passes disk, retained-state and
guest-persistence checks; its Unicode recovery-shortcut failure is corrected.
Portable lifecycle acceptance remains open.
The complete r15 runtime plus the rebuilt compatibility-20 guest now pass
integrated default Vulkan playback (15 seconds, 100%, exit 0), visible moving
video, persistent-file integrity and clean shutdown. The packaged compatibility
update delivered the settings after the manual copies were removed. Both build
pipelines and current launcher CI pass. The final sections record exact artifacts,
the allocation fix, guest locking/unload workarounds, regressions and evidence.
The earlier storage pause is historical; recheck current free space before
recovery operations.
The dated sections below retain prior failures and superseded intermediate states.

Open release gates include camera usability,
portable lifecycle acceptance, physical keyboard/focus and
monitor checks, and host sleep/network/device checks requiring available hardware
or Windows permissions. This AMD laptop also cannot supply the separate Intel,
NVIDIA and ARM64 hardware evidence required by the broader validation plan.
The full-feature document additionally retains implementation requirements for
production accelerated saved sessions, direct application drop placement,
a camera bridge, bridged networking and the ARM64 runtime/guest delivery path.
Automated fixture passes do not close those implementation requirements.

## Candidate and evidence

- Repository: `omacom/try-omarchy-windows`, branch `codex/full-feature-completion`.
- Starting commit: `a566995c6c60b7b7e98289eac44023632c28c43e`.
- Local evidence/artifacts: `C:\cssi\try-omarchy-acceptance\2026-09-13`.
- Runtime build: `34729022320`, artifact `winq-emu-alpha10-source-build`.
- Runtime ZIP SHA256: `d2a3c972d6837730ecb99f3b470af534f0dba8ca7faf3b2dc36be8adb5fbc3df`.
- Guest build: `34731444397`, artifact `guest-candidate`.
- Expected raw rootfs SHA256: `dafc29fe70e74fd6621290c0172a4d0a3b16ad237cb747a14cfa83a31fdca82a`.
- Go 1.27.1 Windows AMD64 archive verified against go.dev SHA256:
  `a3911b5e0e1b1053f25ed0675f4c1c6aad1e2bfcf253df2b9be4caabd2edd95d`.

The original `%LOCALAPPDATA%\TryOmarchy` installation is retained. No VM was
running at discovery. The two old executables in the repository were retained.
Unsigned source candidates remain separate from signed release evidence.

## Host

Windows 11 Business, build 26200; AMD Ryzen 5 5625U, 12 logical processors;
AMD Radeon Graphics driver 31.0.21921.1000. Three MS Idd virtual display devices
are also reported. Initial C: free space was approximately 62 GiB on NTFS.
Windows Hypervisor Platform and Virtual Machine Platform are enabled; the full
Hyper-V role is disabled. Detailed facts are in `host-inventory.json`.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| Runtime archive checksum and source/binary recipe verification | Pass | `runtime-build/verify.py` reports 10 required files and corresponding source verified |
| Runtime version and accelerator inventory | Pass | QEMU 11.0.0; TCG and WHPX listed; real WHPX CPU/GPU boots recorded below |
| Compressed guest and metadata checksums | Pass | `guest-verification.json` |
| Decompressed rootfs stream checksum and length | Pass | `rootfs-stream-verification.txt`: 7,516,192,768 bytes, exact expected SHA256; no extra raw image retained |
| Current launcher build and Windows vet | Pass | Launcher SHA256 `512f728b67190e8f3d441102264780b675f7adbd406e8018e13ab4cae9fce319`; `launcher-hash.json`; vet rerun after fixes |
| Windows automated runtime suite with native UI and clipboard opt-ins | Pass, 342 top-level tests; 9 skipped | `windows-full-retest.jsonl`, `windows-full-retest.stderr.txt` |
| Release helper tests | Pass, 15 tests | `release-helper-final.txt` |
| Fresh combined-candidate WHPX desktop and GPU | Pass | `fresh-r7/vm/shell.log`, `fresh-r7/render-probe.json`; visible desktop and runtime SHA256 match |
| CPU rendering | Pass explicit r12 three-display path and forced GPU-startup fallback | `r12-cpu-three-dpms.txt`, `r12-forced-fallback-guest.txt`, `r12-forced-fallback-probe.json` |
| Native clipboard, transfer window, USB manager, display-window lifecycle | Pass for fixtures | `native-tests.jsonl`; does not prove real guest/device integration |
| Actual native OLE drag into SDL | Pass, three repetitions | `native-ole-drop-tests.jsonl` |
| LAN firewall lifecycle | Blocked by Windows permissions | `firewall-tests.jsonl`: New-NetFirewallRule returned Access is denied; not a pass |
| Real guest text clipboard | Pass both directions | `clipboard-guest-to-host.json`; Windows source and received UTF-8 files have identical hashes, including trailing LF |
| Real guest file clipboard | Pass both directions | `clipboard-folder-host-to-guest.json`, `clipboard-folder-guest-to-host.json`; 40 MiB random file, Unicode names, empty directory, matching hashes |
| Real guest image clipboard | Pass both directions | `image-clipboard-host-to-guest.json`, `image-clipboard-guest-to-host.txt`; distinct size/pixel fixtures |
| Shared folder | r7 fails; r8 fixes live retest | `r8-share-retest.txt`, `r8-share-crud.json`, host hash: Unicode read/write/rename/delete and current/explicit timestamps |
| Guest network | Pass localhost SSH and outgoing HTTPS | `shared/guest-facts.txt`; production QEMU user network |
| Physical USB | Built-in camera discovery/attach/release passes; video usability fails | Guest USB configuration fails with error -32; no video device. External storage/serial hardware remains unavailable |
| Audio routing | Pass host-session playback/capture lifecycle; subjective sound/device-switch tests open | `audio-tone.json`, `audio-capture.json`, `capture-byte-count.txt`, `audio-after-capture.json` |
| Backup and restore | Pass creation, cancellation, full disk hash verification, restored Unicode-path boot on r9 | Persistent fixture hashes also survive disk growth, reclaim and relaunch |
| Awake idle / video CPU | Five-minute samples pass; default Vulkan playback fails | Idle mean host CPU 0.611639%; explicit OpenGL video mean 11.30079% |
| One-hour endurance | Pass r12, 61 samples over 3,613 seconds | `r12-endurance-result.json`; stable process/boot identities, QMP disk status and fixture hashes |
| Host sleep/resume | Pending | Host has not slept or rebooted during this acceptance run |
| Multiple displays | r12 three-output GPU DPMS passes; secondary native rendering observed; full visual/input acceptance ongoing | r10 teardown, r11 stale-input and r12 GL-context sharing fixes; r12-gpu-dpms-cycles.txt |
| Installation move | Pass cancellation, disk-full recovery, redirect boot and retained-source cleanup | move-cleanup-result.json; moved-guest-persistence.txt |
| Full snapshot create/list/restore-as-copy | Pass native UI, Unicode name, 24 GiB whole-disk hash, desktop boot and persistence | r12-snapshot-created.json; r12-snapshot-restored-disk.json; r12-snapshot-restored-boot.txt; r12-snapshot-restored-desktop.jpg |
| Full snapshot rollback and portable lifecycle | Open, storage gate | Disposable restore retained after cleanup approval rejection; see final section |
| Launcher TCP/UDP forwards | Pass localhost round trips | `r9-launcher-forwards.json`; LAN/firewall remains separate |
| Settings repair and diagnostics | Pass real installation | Decline preserves malformed bytes; repair retains backup; diagnostics redact path and omit disks/private key |

An initial test invocation overlapped Go archive extraction and failed to find
standard-library files. It is retained in `toolchain-extraction-race.jsonl` as a
harness preparation failure; the suite was restarted after extraction completed.

## Failures investigated and fixed

The first complete Windows run passed 332 tests, skipped 16 and failed three
saved-session RAM tests. The failures reported `Invalid argument` connecting to
the real `%LOCALAPPDATA%\TryOmarchyIPC` directory. A diagnostic reproduced the
failure with both QEMU and a direct Windows socket client. Moving the fixture
IPC to its own temporary directory succeeded. The existing IPC directory's ACL
includes restricted Codex identities. No ACL or production socket transport was
changed. `startSavedSessionTestQEMU` now owns an isolated, short IPC directory;
the saved-session retest and full suite pass, including verified RAM restoration
in a new QEMU process. This is TCG fixture evidence, not accelerated desktop
saved-session acceptance. Diagnostic observations are in `socket-diagnostic.txt`;
its logging-only PASS labels are not acceptance assertions.

Release-helper fixes use the host PATH separator for test command mocks, remove
native Python's CR from Git Bash runtime-lock rows, and write the checksum test
fixture with LF. Tests require Git's `usr\bin\bash.exe`, not its `bin\bash.exe`
wrapper, which prepends real commands ahead of the mocks. The evidence directory
contains a `tools\bin\python3` shim selecting bundled native Python. All 15
release-helper tests pass with that environment.

The nine full-suite skips are four Linux guest interop tests, two symlink tests
without the required Windows privilege, native OLE (passed separately), the
complete guest transfer fixture (subsequently passed), and firewall (attempted separately,
permission denied). None are included in the 342 passing count.

The PR's three required checks pass at the starting commit. It remains draft,
with no posted reviews or comments when checked (`pr-state.json`). Local changes
have not been pushed, and no release has been published.

## September 13 live desktop continuation

The user explicitly approved all necessary work after the first launch block.
The initial boot selected existing `C:\WINQ-EMU` by documented runtime precedence.
It reached readiness and powered off cleanly, but is not r7 acceptance evidence.
The corrected launch uses `-winq` pointing at a nonexistent test-only path so
the launcher downloads and verifies the explicitly selected r7 runtime.
The separate `fresh-r7` installation booted with WHPX and GPU at 14:24:39 CDT.
It uses 6 GiB RAM, eight vCPUs, and a 24 GiB root disk. Guest compatibility is 19.

The PowerShell QMP helper exposed two real compatibility failures under modern
.NET: variable-sized sockaddr_un buffers caused an invalid-pointer error, and
disposing the completed async result's shared wait handle broke successive
connections. The helper now supplies the full 110-byte buffer and polls bounded
async completion without touching the shared handle. Its isolated regression
passes 20 consecutive connections (`qmp-pwsh-regression.txt`). Windows PowerShell
5.1 refused the test script under execution policy; that runtime remains untested.

An ephemeral SSH key and localhost port 2244 support test commands in the isolated
guest. The private key is local test material, not evidence to upload. Persistent
fixtures live in `/home/omarchy/Acceptance`; expected file hashes are recorded in
`shared/guest-facts.txt`. Port 2244 was added through QMP for this boot and must be
re-added or configured through `-ssh` on relaunch. The shared directory is
`C:\cssi\try-omarchy-acceptance\2026-09-13\shared`.

The confirmed 9p Unicode failure is still open. Creating `Clipboard ä¸ç` on
Windows made `ls /mnt/host` fail with Input/output error. Renaming it to
`Clipboard-ascii` restored directory enumeration. The clipboard copy containing
the same Unicode names passes in both directions. Runtime source inspection
shows ANSI CreateFile/_findfirst/_findnext use in `9p-util-win32.c`; this is a
candidate cause, not yet a tested fix. Guest-written files also showed epoch
timestamps, requiring a focused timestamp check. Original files are retained.

## Initial approval-block record

Automatic approval review rejected the candidate launch command with only
`blocked by policy`. User confirmation was requested for the exact executable,
isolated fresh directory, localhost assets, disabled updates and 24 GiB disk.
The user subsequently approved this action explicitly and the isolated launches described above succeeded. The prepared script also isolates process-local
LOCALAPPDATA beneath the evidence directory, avoiding the real installation's
IPC and host integration. The original installation remains untouched.

The localhost artifact server is running on 127.0.0.1:18080 from the evidence
directory (Python `http.server`). For continued exact-r7 testing, run `launch-candidate.ps1`,
inspect setup and desktop with the computer-use skill, then exercise the
remaining real guest Windows matrix. Keep fixture passes distinct from those
end-to-end results. Do not reboot the host without recording a resume point.

## Remaining coverage constraints

The user permits Windows testing and recreation of test artifacts. Host reboot
would terminate this session and requires a durable continuation record first.
AMD-only results cannot satisfy Intel/NVIDIA or native ARM64 hardware gates.
Full Hyper-V coexistence is not covered by the current feature configuration.
Unimplemented accelerated saved sessions, direct application drop placement,
camera bridge, bridged networking and native ARM64 product support remain the
separate engineering gaps identified in the session handoff (docs/HANDOFF.md). Do not infer their
completion from automated test counts or mediated transfer-window tests.

## Lifecycle and source-fix continuation, 15:00 CDT

- Exact r7 GPU session ran from 14:24:16 to clean guest poweroff at 14:52:52.
  This is approximately 28 minutes, not the one-hour gate.
- `gpu-idle-cpu.json` records 72 five-second samples. The sample includes guest
  display power saving, so it must not be described as five minutes of awake
  desktop idle. `r7-idle-display-off.jpg` and `r7-gpu-desktop.jpg` distinguish
  the powered-off display from the recovered desktop.
- Hyprland reported dpmsStatus=false. The current Lua dispatcher
  `hl.dsp.dpms({action = "on"})` restored its lock screen, and the documented
  trial password unlocked it using QMP input. Shift-only wake was inconclusive;
  normal native wake/input acceptance is still outstanding.
- A separately copied runtime with an experimental UTF-8 manifest booted with
  the same disk. This is not r7 release evidence. It did not fix Unicode 9p
  enumeration (`utf8-share-experiment.txt`) and was cleanly stopped.
- Exact r7 CPU rendering booted at 14:54:41, reached userspace ready at 14:54:53,
  and handled a guest reboot at 14:58:53 by starting a new QEMU process. The
  relaunched guest was ready at 14:59:11. Persistent text and binary hashes
  match before and after (`cpu-post-reboot-persistence.txt`). The product's
  `-ssh 2244 -ssh-key` setup works on both starts.
- `r7-share-failures.json` confirms nested Unicode enumeration EIO and current
  timestamps becoming epoch. Explicit timestamp assignment as ordinary user
  was denied by guest permissions; the root experiment sets the explicit date
  correctly, but current-time handling remains broken. Keep these distinct.
- Draft recipe r8 adds patch 0008 and a native Windows regression for Unicode
  file APIs and timestamp NOW/OMIT/explicit/invalid/NULL semantics. It is not
  yet compiled or live-tested. The public runtime lock remains unchanged.
  The manifest-only experiment must never be published as r7 or r8.

## Recovery and runtime-build evidence, 15:30 CDT

- PR commits fb34bb6 (Windows harness and initial evidence) and 6ad4965
  (runtime sharing fix) are pushed. All three required PR checks pass at
  6ad4965. Runtime build 34764396206 succeeded, including the compiled native
  Unicode/timestamp regression. `runtime-r8-build.log` retains the build output.
- r8 portable ZIP SHA256 is
  `7d4d4a65e175201c2e93a729c7edfa5553744340e1128832a05dffa42b7000ea`;
  source ZIP is
  `6bb3920cea129242bfbc1ab2c251b8d996d80ef2c631a331b62c588973590319`.
  Full archive/source verification passes. QEMU window executable SHA256 is
  `9c017432808a78dbac9eb7a97dca286865974de231d82ad5120f96fd7d7266a2`.
  Extracted path: `runtime-r8`. Live retesting is still required.
- Full guest transfer round trip passes on the physical laptop with exact r7
  against the separate disposable `fresh` disk: production streaming negotiated,
  both transfer windows verified, 31 MiB Unicode files matched, originals and
  both text clipboards retained. `guest-drop-roundtrip.txt` and
  `guest-drop-guest-check.txt` record the host and guest results. The standalone
  QEMU process was cleanly powered off after this test.
- Backup refuses a running launcher and creates no archive. A separate attempt
  during a deliberate source-disk hash read hit the exclusive file-lock guard;
  this was a harness overlap, not a VM that remained running.
- Backup cancellation removes the temporary archive. Low-space protection
  reports required and available GiB without publishing an archive. After space
  became available, full backup succeeded: `r7-persistent-backup.zip`,
  6,491,469,142 bytes, 204 manifest entries. Do not upload this private archive.
- The 24 GiB source disk, archived disk manifest and restored disk all have SHA256
  `84357b2abb206b6501ce6cc549b986db48a1b95e60c240d6ee36183795390c12`.
  Full restore into `Restored ä¸ç` passed. Restore cancellation also left no
  destination or staging directory and preserved backup/source. The CLI restore
  tells users to launch with -dir; the separate recovery-UI shortcut path has
  not yet been tested.
- Fresh setup was cancelled near the end of unpacking. Downloads had already
  completed before confirmation, so interrupted-download acceptance is still
  open. Relaunch of `cancel-download` reached compatibility-19 userspace using
  r7. Its normal uninstaller removed the disposable folder and its own Apps &
  features entry; original installation, current r7 disk and shared files remain.
- Automatic approval review rejected starting a second throttled localhost
  server and separately rejected deleting the obsolete `fresh` disk/rootfs
  files. Both rejections gave only "blocked by policy". Neither action was
  retried through an alternate mechanism; the first test fixture remains.
  The distinct `cancel-download` copy was removed through its tested uninstaller.
- Six-minute GPU idle sample: mean 0.729% of total host CPU, maximum 3.785%,
  equivalent mean 8.75% of one logical processor. This includes guest display
  power saving and is not the awake-desktop idle gate.
- Direct QMP keypresses reach guest evdev, while the tested Windows automation
  injected key produced no guest event. Physical keyboard confirmation remains
  requested. No host sleep or host reboot has been performed.

## Additional live findings, 15:50 CDT

r8 fixes the original sharing failures on this physical AMD host. Native QEMU
process identity is recorded in `r8-live-process.json`. Both current-time and
explicit-time operations pass with appropriate guest permissions; Unicode
creation, enumeration, reading, rename, file deletion and directory removal
pass, including host-side checksum comparison. Text clipboard A/B/A repeats
also pass after the r8 boot.

A separate r7 failure appeared when booting the exact restored disk from
`Restored ä¸ç`: QEMU could not open `vm/qemu.log`, then CPU fallback failed for
the same reason. The disk itself has the expected checksum. A packaged-binary
regression also reproduces Unicode path failure in r8 qemu-img. Commit 3ca83dc
adds process UTF-8 manifest settings and initializes the UCRT character locale
before file options are parsed. Numeric locale remains unchanged. This follows
Microsoft's [UCRT UTF-8 documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setlocale-wsetlocale#utf-8-support)
and [per-process code page documentation](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).
Recipe r9 is building in run 34766175917; it is not yet a tested fix. Public pins
remain unchanged. All three required PR checks pass at 3ca83dc.

Audio evidence uses NAudio Core/Wasapi 2.2.1 packages verified against NuGet's
catalog SHA512 hashes, installed only under the evidence tools directory.
QEMU's capture session was inactive at idle. An eight-second quiet generated
PCM tone reached the QEMU Windows render session (active, nonzero meter, then
inactive). A five-second guest capture opened and closed the Windows capture
session. A separate one-second byte-count probe delivered exactly 96,000 bytes
at 48 kHz, mono, s16. Audio was discarded, not retained. The count-limited
pw-record command returned 1 despite delivering the requested sample count;
this diagnostic exit is retained rather than silently reported as zero.
Both Windows audio sessions were inactive after capture. These facts do not
replace a human listening test, headphone switching, or visual microphone-icon
confirmation.

The guest is temporarily set to Stay Awake using its own toggle for the
five-minute awake-idle CPU sample. Restore Allow Idle after the measurement.
Host S3 and hibernate are supported. Reading host wake timers requires elevation;
no host sleep/reboot has occurred. User availability for physical keyboard,
UAC/firewall, and host sleep/wake checks was requested while testing continues.

September 13 afternoon continuation: malformed Settings repair was exercised on
`Restored ä¸ç`. Declining preserved the exact malformed file. Accepting retained
it under `preferences-before-repair-3597579487/settings.json` with SHA256
`215579e970e8fb878cd2546bad369026d71bb6d856f125195c494112baf37c60`, then
restored defaults. The native Settings form showed automatic rendering, one
display, no shared folder and no forwards; its screenshot is retained.

The five-minute r8 awake-idle sample contains 60 readings, with mean total-host
CPU 0.611639%, maximum 2.15745%, and mean single-core-equivalent 7.33967%.
This is the awake-idle result, separate from the earlier mixed-DPMS sample.
TCP and UDP temporary QMP loopback forwards each returned the exact Unicode
payload and were removed successfully (`r8-network-loopback.json`). This does
not certify persisted launcher forwarding or LAN/firewall behavior.

Video acceptance found a new failure: default mpv Vulkan playback on Venus
(AMD Radeon Graphics) reports no suitable host-visible memory and repeated
VK_ERROR_OUT_OF_HOST_MEMORY, barely advances, and ignores SIGINT/SIGTERM.
The test process required SIGKILL. Explicit OpenGL playback reached the end
of the 15-second 1280x720/30fps clip with one dropped frame. A five-minute
OpenGL run is in progress. No mpv preference was changed to hide the default
failure. Logs are retained under the test share.

The r8 boot journal also contains one early disk write error before ext4
mounts read/write. QMP currently reports disk io-status ok. This remains an
unresolved diagnostic; the run must not be described as a clean stability pass.

Run 34766175917 built r9 but CI smoke failed because the staging directory
contained only the console QEMU binary, omitting qemu-img and the windowed
binary. The packaged archives passed verification locally. After correcting
that staging and replacing unreliable redirected-stdio QMP with the existing
bounded socket harness, all three r9 binaries pass real Unicode paths locally;
r8 fails the same test as the negative control. Commit 9c2565c contains the
harness corrections. Replacement CI run: 34767422097. The local r9 archives
are from 3ca83dc/run34766175917 and have not yet booted the restored installation.

R9 restored installation reached userspace in CPU mode at 16:10:54. The
bundled runtime update committed, installed executable SHA256 is
`0c0e4a02dc5da7838294852a1e9d9293aea6f9c22e51f7fe64064ad763f0b959`,
and both persistent fixture hashes match. Disk growth from 24 to 26 GiB
completed inside ext4. The restored boot has no disk I/O error in dmesg.
Both launcher-configured TCP and UDP loopback forwards passed. Hyprland reports
two enabled outputs; independent native-window visibility still needs proof.
Reclaim prepared 5448 MiB, completed after clean shutdown, and the native result
reported 5.4 GiB less allocated Windows disk space. Persistent files matched
before shutdown; verification after relaunch remains part of the next check.

The first two-display GPU attempt exposed a launcher bug: JSON `hostmem` was
sent as a string (`"4G"`), while QEMU requires an unsigned integer byte count.
The launcher now carries that setting as uint64 and its regression checks JSON
typing. It also retains a bounded stderr tail in shell.log before fallback,
so earlier startup failures survive retry. Native reproduction proved the
original argument failure and the retained diagnostic.

After correcting the argument, GPU startup progressed further and exposed a
runtime assertion in surface_gl_create_texture: a disabled secondary output
had destroyed its shader but retained scanout_mode. A delayed disable attempted
to recreate a texture with that null shader. Recipe r10 clears the scanout and
destroys its framebuffer while the context still exists, before destroying the
secondary window. The extracted real C helper regression aborts on r9 and passes
on the correction, including three disable/reactivate cycles. Physical runtime
acceptance of r10 is still pending. CPU fallback preserved the installation.

The post-change Windows suite passed 336 top-level tests and skipped 16 with
interactive opt-ins disabled (`windows-post-multidisplay-tests.jsonl`). This is
separate from the earlier 342-test interactive run. All required PR checks pass
at 97710cf. Diagnostics creation on the real running installation passed:
installation path redacted, no disk images/private key, original startup failure
retained. Its 13 entries and digest are in `diagnostics-live-verification.json`.

Native close-button activation opened the shutdown confirmation while QEMU
remained running. The UI automation helper could not address the cross-process
owned dialog, so accepting/declining remains a manual gate; the file named
`close-declined-running.json` only proves the guest was still running, not that
the decline action succeeded. QMP was used for the subsequent clean shutdown.

Three-display CPU fullscreen startup failed repeatedly after readiness, without
UI input. An attached debugger recorded 0xc0000094 (integer divide by zero) at
QEMU module offset 0x3390e4. The packaged debug symbols resolve this to
`handle_mousemotion`, ui/sdl2.c:543. Stale SDL window IDs resolved to NULL and
incorrectly matched an inactive output; the handler divided by a zero window
size. Recipe r11 rejects stale IDs and guards zero sizes/missing surfaces in
both motion and button handlers. The real extracted C regression fails on the
old lookup and passes the correction, including normal coordinate scaling.
The launcher now also records nonzero QEMU exit status and no longer describes
an exit without a shutdown event as a confirmed guest poweroff. Windowed
three-display CPU mode remained running during this investigation.


### Combined r11 acceptance preparation

Required PR checks pass at 57c837f. Runtime r10 build 34768261746 passed;
combined r11 build 34768987337 is in progress. The locally built r11 launcher
includes the nonzero-exit diagnostic correction and passes Windows vet.
Public runtime/guest pins and signed release artifacts remain unchanged.

A direct Windows Vulkan probe establishes the default-video allocation cause
on this driver. A 64 KiB ordinary transfer-source buffer allows memory types
03, including three host-visible types. The otherwise identical buffer with
OPAQUE_WIN32 external-memory support allows only type 0, which is device-local
and not host-visible. This matches the guest's memoryTypeBits=0x1 failure.
Venus currently requests Win32-exportable buffers because its host-visible
allocations must cross the virtualization boundary. Removing that declaration
without replacing the memory-sharing design would not be a validated fix.
Evidence: `probe-vulkan-memory.py` and `host-vulkan-buffer-memory.json`.
The probe creates/destroys buffer objects but does not allocate GPU memory or
change drivers. Python Vulkan bindings are isolated under the test tools.
See the [Vulkan external-buffer contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryBufferCreateInfo.html)
and [buffer memory requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetBufferMemoryRequirements.html).

The five-minute explicit OpenGL video run completed with exit 0, mean host CPU
11.30079% and maximum 13.82146%. This does not close default Vulkan playback.
The r8 early disk write error has not recurred on restored r9 boots, whose
persistent fixture hashes remain correct; its original cause is unresolved.
The actual snapshot Create operation refused with 34.7 GiB required and 9.1 GiB
available. The store contains zero entries after failure, with no pending
snapshot directory. Native dialog evidence: snapshot-low-space.jpg. This proves
safe low-space handling, not a successful full snapshot/rollback round trip.

Runtime r10 from build 34768261746 passed full local archive/provenance
verification against the 97710cf recipe. Installed windowed QEMU SHA256 is
b952180c4c4c8eb22a6c044b81671c4172f72ad9d2656af12dcfb7da31121c45.
The restored Unicode-path installation reached GPU userspace at 16:50:33 and
committed its authenticated runtime update. Three guest DPMS off/on cycles
returned both enabled outputs with unchanged persistent fixture hashes and no
QEMU assertion. The helper's final no-error grep initially encountered a CRLF
shell terminator; a separate corrected kernel scan passed and the helper was
normalized to LF for the next run. Evidence retains that harness failure.
Only one native window was enumerated after readiness, so this does not yet
establish two visible native outputs. R11 is the next integrated retest.

### Resumed combined runtime round

The host wall clock now precedes earlier log timestamps. Endurance timing uses
Stopwatch elapsed time rather than wall-clock subtraction. The prior QEMU and
localhost asset server were no longer running; the original loopback-only
asset service was restarted. No host reboot or clock change was performed by
this acceptance agent. The active laptop GitHub credential is tsouth89 with
repository push permission; the earlier lab handoff's btsouth account is not
present in this session.

Runtime r11 build 34768987337 passed CI and full local archive verification.
The installed executable SHA256 is
f155a361d4d275b0c81ac1e595dee1e515187e1fff98de1221aea4695d3e42b5.
Three-display fullscreen GPU boot reached userspace at current host time
12:06:53, committed its authenticated runtime update, and passed three DPMS
cycles with all guest outputs enabled and persistent hashes intact. All three
native windows remain present: the stale-event fix closes the disappearing
window symptom as well as the immediate crash in this run.

Visual acceptance nevertheless fails: secondary native GPU windows are black,
while `grim -o Virtual-3` captures the correct rendered guest desktop. Native
and guest screenshots are retained. R12 explicitly makes the primary context
current before creating a secondary SDL GL context and enables sharing with
that context, including after display recreation. The extracted real window
creation regression fails on r11 and passes the proposed correction through
three recreation cycles and the software path. Physical r12 acceptance is
pending; r11 is not a complete multi-display pass. SDL's documented context
sharing behavior is described in https://wiki.libsdl.org/SDL2/SDL_GLattr.

### Storage block during the resumed recovery round

R12 runtime build 34770918856 is compiling commit 53093ed; all required PR
checks at that commit pass. A native probe using the bundled SDL2 library and
this AMD GPU confirms that a recreated secondary GL context cannot see the
primary texture without explicit sharing, and can see it with sharing enabled.
Evidence: probe-sdl-context-sharing.py and native-sdl-context-sharing.json.
The complete r12 guest/visual retest remains pending.

R11 fullscreen monitoring recorded ten successful samples (minutes 0 through
9), with stable QEMU/guest boot identity, working SSH and correct persistent
fixture hashes. It was deliberately ended by QMP powerdown because secondary
windows already failed visual acceptance and recovery work needed the lifecycle
listener. The monitor's subsequent missing-process error is an intentional-stop
consequence, not a spontaneous crash or a one-hour pass. See
r11-endurance-intentional-stop.json and the endurance samples/result files.

The real move of fresh-r7 into Move 世界/TryOmarchy passed cancellation: the
source remained, no destination was published and staging was empty. Retrying
the full move completed copying but hit ERROR_DISK_FULL while reading the new
disk for final verification. C: reports zero free bytes. The journal remains
pending in phase verified, redirects remain empty, and both source and new
copy are retained. Do not delete either or edit the journal to force activation.
After space is available, normal launcher recovery must reverify the new copy,
activate the redirect, boot successfully and verify persistent files before
retained-source cleanup can be tested.

NTFS allocated-range queries show the destination disk still sparse, with
6,839,468,032 allocated bytes for its 25,769,803,776-byte logical disk; its
factory rootfs uses 5,915,803,648 allocated bytes. The source counterparts use
7,148,994,560 and 5,931,794,432 bytes. These observations do not establish a
space-estimation defect: the reason the remaining host space disappeared is
not yet determined. Source disk inventory SHA256 is
f04a8e8ae3829d35ada8b69cd89ceda8229676acc91f6e18f18f93cea66577f9.
The source is authoritative until recovery activates the destination.

Automatic approval review rejected removal of the redundant runtime-r10
extraction with the reason "blocked by policy". It was not retried through
another tool. The user was asked to free at least 15 GiB outside the acceptance
folder or provide another drive. All guest processes are stopped. No host
reboot, driver change or clock change was performed. Writes and further guest
boots are paused until storage is available; release acceptance remains blocked.
This report update was committed through the GitHub API because C: is full.
After freeing space, fast-forward the local branch before recording more work.

### Storage recovery and r12 integrated retest

The user removed the redundant 6,491,469,142-byte acceptance backup ZIP.
Normal move recovery then verified and activated `Move 世界/TryOmarchy`.
Launching through the old `fresh-r7` path followed its redirect, booted the
moved guest, and verified both persistent fixture hashes. After clean shutdown,
the product's move-cleanup operation removed the retained source. The journal
now contains only the redirect, with no pending or retained move. C: subsequently
reported 33,979,215,872 free bytes. The original user installation is untouched.
Evidence: move-recovered-boot-state.json, moved-guest-persistence.txt,
move-cleanup-preflight.json, move-cleanup-result.json and move-cleanup-success.jpg.
Historical source logs were preserved in r7-history before cleanup. The local
branch was fast-forwarded to the remote evidence commit after space returned.

Runtime r12 build 34770918856 passed CI and full local archive verification.
The installed windowed executable SHA256 is
1a9a80c22fa7f43da632675fe7afc4c888db533d325703614584826882b0bb9d.
The restored Unicode-path guest reached readiness at current host time 12:37:26
with three fullscreen GPU outputs. Native display 3 showed the desktop instead
of a black surface. Three explicit DPMS off/on cycles returned all three guest
outputs enabled with unchanged persistent fixture hashes. A later native display
2 capture showed the rendered guest lock screen. Guest idle poweroff was disabled
for the awake endurance portion; user unlock was requested without automating
authentication. Desktop visual confirmation after those cycles remains pending.

The r12 one-hour endurance monitor started at 17:42:42 UTC. It uses monotonic
elapsed time, checks process identity, QMP running and disk I/O state, guest boot
identity and both fixture hashes every minute, and records host CPU, working set
and C: free space. It fails below a 2 GiB host reserve. No one-hour pass is claimed
until r12-endurance-result.json records completion. Vulkan playback and remaining
hardware/lifecycle gates still prevent release acceptance.

All three native r12 windows subsequently showed the rendered lock screen after
power cycling (r12-display1-after-dpms.jpg through r12-display3-after-dpms.jpg).
This confirms rendering, not physical keyboard focus or a multi-monitor layout.
Fresh r12 shared-folder Unicode CRUD and launcher TCP/UDP forwarding round trips
also pass: r12-share-crud.json, r12-share-host-hash.json and
r12-launcher-forwards.json.

The next portable round has an isolated source build with the combined r12 and
guest19-r2 manifest embedded. Its launcher SHA256 is
6e5b5605da2d45731fd3e8e090d16e708c136f59f31248c6db068b3da5898640.
Candidate source, combined assets and build identity remain under the acceptance
root. Assets use hard links to existing verified downloads to avoid another
large copy. Public launcher defaults and release pins remain unchanged. This
build preparation is not portable creation or boot acceptance.

The native AMD Vulkan host-import transfer probe passes: a 4,096-byte-aligned
host allocation imported as memory type 1 was bound to a buffer, filled by GPU
commands, synchronized with a transfer-to-host barrier and fence, and all 65,536
bytes matched the expected pattern on CPU read. Evidence:
probe-vulkan-host-import-transfer.py and host-vulkan-host-import-transfer.json.
This narrows a possible implementation path; it does not fix guest Venus.
The current runtime still forces OPAQUE_WIN32 external buffers, which this driver
restricts to a memory type lacking HOST_VISIBLE. A correction must preserve
buffer/image handle compatibility, mapping ownership and synchronization across
the renderer and QEMU. The probe follows the Vulkan
[host-pointer import contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html)
and [pointer memory-type query](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryHostPointerPropertiesEXT.html).

After the user reported logging in, native desktop control resumed and the
unlocked primary desktop rendered correctly. The earlier 12-second input probe
left little capture time after Windows tool startup; an extended 45-second probe
was therefore run before drawing a conclusion. It captured native mouse button
events and the control QMP `b` key events, but not the desktop tool's injected
`a` key. This is recorded as an automation limitation, not a proven physical
keyboard defect. User-reported login is separate from full shortcut/focus testing.
Evidence: r12-native-key-events-long.json and r12-unlocked-primary.jpg.

The first r12 OpenGL video attempt failed because the restored copy lacked the
earlier video fixture; no video played in that attempt. A new 15-second 720p30
test pattern was generated (SHA256
5594b658a7d6c8f723e083a14b63da60fb8bccf3133773bea3aab3f0f6bab97e).
The five-minute retest uses explicit OpenGL with audio disabled and a monotonic
CPU sampler. During playback, the same mpv window moved from guest display 2 to
3 to 1; native screenshots confirm video rendering on every output. This is not
a Vulkan playback pass. Final playback and endurance results are recorded when
their respective monitors finish.

An independent eight-second quiet tone during the r12 session passed guest
PipeWire playback and host QEMU audio-session verification. Render activity
transitioned active/inactive with peak 0.00268815; capture remained inactive.
Evidence: r12-audio-guest.txt and audio-r12-tone.json. No microphone audio was
recorded in this check.

The r12 OpenGL video retest completed normally at 317.7 seconds with exit code
0. Its 60 five-second CPU samples averaged 14.2079% host CPU (maximum 16.5549%).
The first post-playback endurance sample covering an idle minute was below 1%.
The separate five-minute post-video idle sampler remains pending at this point.

The real Backup command refused while the guest was running, reporting the
active lifecycle port. No destination ZIP was created and the same QEMU PID
remained running. Evidence: r12-running-backup-refusal-visible.jpg and
r12-running-backup-state.json. The earlier screenshot without the `-visible`
suffix captured an occluded surface and is not visual dialog evidence.

The guest virtual NIC was deliberately disconnected for three seconds and
reconnected through QMP; SSH recovered with the same guest boot identity and
outbound HTTPS returned 200. This does not test host adapter changes or LAN
firewall behavior. Windows Central Standard Time/en-US mapped to guest
America/Chicago/en_US.UTF-8 with US keyboard configuration. Evidence:
r12-guest-link-recovery.json and r12-host-locale-integration.json.

### Completed r12 endurance, shutdown, CPU fallback and test-copy cleanup

The r12 endurance monitor passed all 61 samples over 3,613.46 monotonic seconds.
The same QEMU process and guest boot identity survived idle time, video playback,
display changes, audio output and the brief virtual-link recovery test. QMP disk
I/O status and both persistent fixture hashes remained valid. Final kernel error
scan contains only the unsupported Intel TDX message on this AMD host; no block
I/O or ext4 error was reported. The five-minute post-video idle check averaged
0.6242% host CPU across 60 samples (maximum 1.7890%).

Alt+F4 from a secondary display opened shutdown confirmation. The desktop helper
could inspect the dialog through its primary QEMU owner, but rejected mouse input
because the actual dialog belongs to the launcher. Normal dialog keyboard input
worked: Return on default No dismissed it without stopping the guest; on the
second prompt Left selected Yes and Return confirmed it. The launcher recorded
graceful shutdown, guest poweroff and clean exit. Evidence: r12-close-no-result.json,
r12-close-yes-result.json, r12-close-confirmation.jpg and r12-endurance-history.

R12 then booted three fullscreen CPU-rendered outputs, passed three DPMS cycles,
rendered all three native desktops and shut down cleanly. A separate auto-render
launch set SDL_OPENGL_LIBRARY to a deliberately nonexistent test path only in
that process environment. The GPU attempt failed with 0xc0000005 as induced;
the launcher logged the failure, automatically booted CPU rendering on attempt
2, recorded a CPU probe result and reached a visible desktop with intact fixture
hashes. The environment override was restored after launch. The guest then shut
down cleanly. This is controlled failure-recovery evidence, not a spontaneous
r12 GPU crash. See r12-cpu-history, r12-fallback-history and the corresponding
launch scripts/results.

After preserving those logs and screenshots, the product uninstaller removed
only `Restored 世界`. The moved installation and original user installation
remain present. The path-scoped preflight and result are in
r12-retire-restored-preflight.json and r12-retire-restored-result.json, with
r12-uninstall-success.jpg. C: reported 48,624,910,336 free bytes afterward.
This target is distinct from previously approval-rejected cleanup targets.
The moved installation then updated through the isolated combined-payload
launcher, booted with the verified r12 runtime and both persistent hashes intact,
and shut down cleanly. The combined manifest digest is
`089a3e2e9e45386581f40bfa3c34f866f49e4886c6b7b1e59353a4accd8f3fdd`;
the isolated launcher SHA256 is
`6e5b5605da2d45731fd3e8e090d16e708c136f59f31248c6db068b3da5898640`.
Public release defaults remain unchanged. Evidence: combined-r12-moved-runtime.json
and combined-r12-moved-persistence.txt. Full snapshot creation with the Unicode
name `r12 baseline 世界` is now running through the native UI.

### Full snapshot creation and independent boot passed; storage gate

The native snapshot UI created and listed `r12 baseline 世界`, ID
`184b4dabfb0abc1c2fbbecb75d30bb61`. The archive is 6,510,965,002 bytes with
SHA256 `a8aada049e0590441473b90165872ca2ad61c487d81ae576baed95ba154dde88`.
Restore as copy completed through the native folder picker into
`C:\cssi\try-omarchy-acceptance\2026-09-13\OmarchySnapshot-20260913-140302`.
An independent SHA256 of the restored 24 GiB disk matched the archive manifest:
`f00885b1c334c2ced5087d6a735359b5b6558683c40cd84b68046bb4bafb953f`.
The restored guest reached a visible desktop, systemd reported `running`, both
persistent fixture hashes matched, and QMP poweroff completed cleanly. Logs are
retained separately under `r12-snapshot-copy-history`. Evidence includes
`r12-snapshot-restore-success.jpg`, `r12-snapshot-restored-disk.json`,
`r12-snapshot-restored-boot.txt` and `r12-snapshot-restored-desktop.jpg`.

Automatic approval review rejected the scoped deletion of this new disposable
restore with only `blocked by policy`. No deletion ran and it was not retried
through another mechanism. The copy remains intact and stopped, as do the moved
source and completed snapshot. C: subsequently reported 9,871,478,784 free bytes.
The restored copy occupies approximately 13.22 GB of allocated storage. Another
full restore for rollback or portable staging would exceed the observed budget;
these operations were not started. This is a cleanup/space gate, not a failed
rollback or portable result. No host reboot, release publication or public pin
change occurred.

To continue, reclaim the disposable copy above (and, if needed, the obsolete
test-only `fresh` copy already identified in earlier cleanup requests), then
verify actual free space again. Keep `Move 世界\TryOmarchy`, its completed
checkpoint, and the original `%LOCALAPPDATA%\TryOmarchy` installation. Next:
write a post-snapshot marker in the moved guest, stop it, exercise active rollback,
verify the marker disappears and the previous disk is retained, then run portable
creation, boot, changed drive letter, update and recovery. Physical USB removal,
another PC and the other graphics/architecture configurations remain distinct
hardware gates. The known Vulkan and camera failures and unfinished full-feature
implementation requirements still prevent release approval.

### Targeted renderer engineering after the acceptance stop

The next work targets the Vulkan memory-sharing failure, without creating more
guest copies. Native capability probes show that OPAQUE_WIN32 and HOST_ALLOCATION
have disjoint compatible-handle masks (`0x2` and `0x80`); combining them is invalid.
Host-importable RGBA8 linear and optimal images and transfer buffers report
host-visible types 1 and 3 (`0xa`), while Win32-exportable equivalents report
only device-local type 0 (`0x1`). A 64 KiB pagefile-backed Windows shared section
was imported into Vulkan and filled by the GPU. Both independently mapped views
contained the correct bytes after closing the original section handle. These
are native backend building blocks, not a guest playback pass. Evidence:
venus-handle-compatibility.json, venus-image-compatibility.json and
venus-shared-section-import.json.

The renderer handle audit found a separate concrete prerequisite bug:
`os_get_win32_handle_from_fd` called UCRT with a synthetic token before consulting
the token table; `mmap` also treated a failed CRT descriptor as a raw HANDLE.
Runtime r12's actual DLL fails the new native regression: both wrapped section
mappings fail and seven CRT invalid-parameter callbacks occur. The test catches
these callbacks only in its own thread and restores the previous handler.
`venus-native-handles-before.json` records the result. This is distinct from the
AMD memory-type incompatibility.

Commit 18d5ae6 adds the r13 handle correction: resolve wrapped handles first,
reject stale tokens without CRT calls, close duplicate handles when token
publication fails, and make mapping use the same resolver. A compiled regression
executes the actual extracted C functions. The unpatched source aborts the test
on its first synthetic-token CRT call; the corrected source passes lookup,
mapping ownership, stale-token and failure-cleanup checks. It ran using the
existing guest's compiler, with no new guest image or desktop acceptance loop.
Evidence: venus-handle-regression-before.txt and venus-handle-regression.txt.
The r13 source-built candidate is in Runtime run 34777622186. Native candidate
validation is pending; public release pins remain unchanged.

The full allocation fix still needs buffer/image creation and requirements to
use compatible backing, shared-memory allocation and blob import/export, and
correct mapping lifetime through memory, device and context destruction. The
existing Vulkan 1.3 device-buffer requirements dispatch also needs to apply the
same creation-info transformation as actual buffer creation. Camera capture and
the other previously listed feature/acceptance gates remain open.

### Active rollback and Unicode shortcut correction

The active rollback of `r12 baseline 世界` completed. Before first boot, the
active disk SHA256 was exactly the snapshot hash
`f00885b1c334c2ced5087d6a735359b5b6558683c40cd84b68046bb4bafb953f`.
The retained disk SHA256 exactly matched the pre-rollback hash
`763b49f02584da4d6878b92dbfd798105182f3f1f7dc53c608fc354f6ded699d`.
Retained data is under
`Move 世界/TryOmarchy/.snapshot-rollback-bfece49d03232a236788356d7c5cbef0/data`.
The rolled-back guest booted (ID `8a3c7cf6-9c02-4529-8d5e-5d634bfb34c7`),
the post-snapshot marker was absent, both permanent fixture hashes matched,
and Hyprland was running. The screenshot shows its idle screensaver. The guest
cleanly powered down at 14:43:49. No Windows host restart was used.
Evidence: r12-rollback-disk-hashes.json, r12-rollback-guest.txt,
r12-rollback-desktop.png and r12-rollback-shell.log.

The completion dialog reported a recovery-shortcut failure. This reproduced
with WScript.Shell's TargetPath setter on existing Unicode paths; an ASCII
candidate path passed. The implementation now uses IShellLinkW and IPersistFile
for shortcut creation and reads ownership through the Unicode interface before
move or uninstall. It also fixes the normal installation and Settings paths.
Regression tests verify Unicode target/arguments/directory, moved ownership and
preservation of another installation's shortcut. The two actual retained-state
links were repaired and their metadata verified without copying a VM or
replacing its launcher. Evidence: r12-snapshot-rollback-result.png,
r12-rollback-shortcut-repair.txt and unicode-shortcut-regression.txt.

The focused Windows group passed 91 tests with one skipped. Its first run had
one transient Access denied failure opening the move fixture's disk; that test
passed in isolation and the complete focused group passed on retry. Both runs
are retained in unicode-shortcut-focused-tests*.jsonl. Windows vet with the
repository's required `-unsafeptr=false` setting passed; unrestricted vet reports
the pre-existing Win32/COM pointer-contract diagnostics.

### Runtime r13 native verification

Runtime build 34777622186 completed successfully. Both runtime and corresponding
source archives passed runtime-build/verify.py. SHA256:

- Runtime: `80197a8739052408a88a5ba0d147405aadd84e42fd7cfbe6dea6ed2dd3b6bf9d`.
- Source: `d193e526b7c1156141f8a72e4a33cfb706325045910232818a145eb7484be179`.

The actual r13 renderer DLL passes the previously failing native regression:
two section views, 65,536 verified bytes, no invalid CRT callbacks, correct stale
token rejection, and ordinary file mappings preserved. Evidence:
venus-native-handles-after.json. This closes the handle bug, not the AMD Vulkan
memory-type incompatibility. Public release pins remain unchanged.

A short guest smoke used the newly built shortcut-corrected launcher
(SHA256 `39f76eceb9c65b7c61d078f103dff940f00e2879c33022013f15cf8d573252dd`)
and explicit user-managed runtime-r13, against the existing rolled-back guest.
The launcher retains the isolated combined-r12-guest19 manifest; no release
manifest was repinned. QEMU PID 15900 ran from runtime-r13/bin, boot ID
`fc6a10c5-de25-415d-ae6a-137b489aeb00`, GPU accelerated on attempt 1.
Default mpv Vulkan again failed at approximately 1% playback with
VK_ERROR_OUT_OF_HOST_MEMORY. Its timeout requested exit but teardown hung;
the specific test player was killed. The guest remained responsive. Explicit
OpenGL then played the complete 15-second fixture and exited 0; both permanent
fixture hashes remained correct. Logs: r13-default-video.log,
r13-default-video-console.txt, r13-opengl-video.log and
r13-opengl-video-console.txt. This confirms that the r13 handle fix does not
resolve the remaining Vulkan allocation path. No fallback preference was saved.

Release decision remains **NO-GO for the full-feature scope**. Active rollback
is now proven and the Unicode shortcut bug is fixed. The remaining work is
implementation and focused acceptance: AMD Vulkan memory sharing, camera
capture, the previously listed missing full-feature paths, portable lifecycle
and hardware-specific gates. Repeating endurance or creating more VM copies
will not close those implementation blockers.

### Continued Vulkan implementation: r14 and r15 engineering

After the user instructed continued work, r14 implemented pagefile-backed host
memory import for internal Venus allocations, SHM blob export, matching resource
requirements, and mapping lifetime through vkFreeMemory/device destruction.
Renderer-only build 34779634209 passed compilation and the native handle test.
Its DLL SHA256 is
`07574f25d22d7f4d0826fe69269cd99fea93c4e82ffcdafc491c159fe9ee010a`.
For local engineering only, that DLL replaced libvirglrenderer-1.dll in the
external runtime-r13 extraction. This mixed directory is not a verified r13
release archive; the original verified r13 ZIP remains intact. No bundled
installation runtime or public pin changed.

Native GPU tests now pass actual allocation, image clear, image-to-buffer copy,
and all 65,536 readback bytes for both linear and optimal host-imported images.
Evidence: venus-host-image-linear.json and venus-host-image-optimal.json.
The guest-side scripts/vmtest/venus-memory.c also passes through the real
Virtio-GPU Venus AMD device for buffer fills and optimal image clear/copy,
including mapping, matching legacy/maintenance4 requirement masks, GPU fence,
byte verification and complete teardown. Evidence: r14-guest-memory.txt and
r14-guest-image.txt. These establish the new memory transport, not playback.

Default mpv no longer reports its previous allocation errors with r14, but
stalls creating its swapchain. A symbolized guest stack identifies
wsi_select_memory_type(req_props=DEVICE_LOCAL, type_bits=0xa): the all-host
resource declaration removed all device-local choices. Evidence:
r14-default-video.log, r14-player-stacks.txt and r14-symbol-stacks.txt.
The 27 MiB matching Venus debug symbols were fetched to the guest's /tmp,
with a bounded download; no full debug package or VM copy was created.

The r15 correction keeps ordinary and host-importable native resource variants
until binding and merges compatible memory requirements without losing
device-local choices. Dedicated allocation selection and both binding APIs
select the corresponding native object. Renderer build 34780579812 is pending
at this checkpoint. Earlier run 34779610100 failed its patch digest because of
Windows line endings; commit 456c2af corrected the digest before any build.
Run 34780030447 additionally passes the compiled actual host-allocation helper
regression, including alignment, handle cleanup, incompatible memory types,
overflow, independent-view lifetime, and driver-before-backing free order.

r15 renderer build 34780579812 succeeded. Archive SHA256 values:
renderer-bin.zip `aebd91f5eb0a7a33f2de8bd1f1b74e1e7b32f0e2132ec5315ddebae414f4dd30`;
renderer-source.zip `209560b592afd808fb1bf5ab42618c51ec549801fec55eb3b8c3123cf337718e`.
All four guest memory modes pass: host buffer, dedicated host image, dedicated
device-local image, and BindMemory2. Both requirement APIs return mask 0xb,
preserving device-local memory. Native DLL handle regression also passes.
Evidence: r15-guest-memory.txt and r15-native-handles.json.

Default mpv still stalls (bounded test exit 137). The new symbolized trace shows
an asynchronous Venus presentation/acquisition mutex deadlock, after successful
swapchain creation. VN_PERF=no_async_present plays the full video but crashes at
thread teardown after driver unload. Adding the documented Vulkan loader option
VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1 completes all 15 seconds and exits
0, without preloading libraries or changing the selected Vulkan renderer.
These are diagnostic environment overrides, not yet packaged guest defaults.
Evidence: r15-default-video.log, r15-symbol-stacks.txt, r15-sync-crash.txt,
r15-loader-video-result.txt. Packaging and default-session verification remain.

### Default session playback and follow-up build failures

After installing the exact compatibility-20 environment files into the existing
compatibility-19-r2 disk and restarting the guest, UWSM's systemd user environment
contains both settings. A systemd-run application with no Vulkan environment or
renderer overrides plays the 15-second video to 100% and exits 0; one initial
frame drop is reported. Evidence: r15-session-video-result.txt. This is a manual
overlay acceptance run, not a boot of the rebuilt guest image. Visual inspection
later encountered the guest lock screen, so it does not yet prove visible video.
The Vulkan loader setting is documented at
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md.

Complete runtime build 34781229515 is in progress. Guest candidate build
34781351688 failed safely on repository lock drift: dua-cli 2.44.0-1 to 2.45.0-1,
gcr-4 4.4.0.1-1 to 4.4.1-1. Patch 0065 updates only those observed package pins;
rebuild 34781616541 is in progress. Package signatures remain required.

Windows CI additionally reproduced a shortcut ownership bug: the input target
used C:\Users\RUNNER~1 but IShellLinkW returned C:\Users\runneradmin. Literal
comparison silently skipped the owned link. Commit 7a15327 compares file identity
as well as paths, retaining literal matching for absent targets. Focused local
shortcut tests pass; CI validation is pending. Earlier local repeated testing
also encountered one transient sharing violation deleting its temporary test
link; a subsequent 30-run diagnostic sequence passed.

Automatic approval review rejected deletion of the completed rollback's retained
folder .snapshot-rollback-bfece49d03232a236788356d7c5cbef0 with 'blocked by policy'.
No cleanup occurred, and no alternate deletion route was attempted. C: still has
approximately 26 GiB free. Current portable creation's backup/raw-staging space
requirements exceed this budget, so no further large portable copy was started.

### Integrated r15 / compatibility-20 acceptance, 16:03 CDT

PASS: complete runtime build 34781229515, recipe winq-emu-alpha10-source-r15.
Runtime ZIP SHA256: `8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc`.
Source ZIP SHA256: `2fa9fa8d57cccbd703523935a9fd44a5b4d6ed82f8259b7302d2303a55a65dd6`.
Downloaded archives passed runtime-build/verify.py and the actual DLL handle test.
Installed renderer DLL: `02e0d58a2827740e499ba4585f4c1900d21cabb0a713161c102578a18c31500d`.

PASS: all four jobs in CI run 34781901160, including complete guest build/boot,
compatibility 20 and Vulkan environment smoke. Factory rootfs SHA256:
`64f37e2a4b5f8ce88ba117177e7d7a1a24a98326c5fc4e5fc2210895f5463ca2`.
Compressed rootfs: `e5495ccd5ec1fc8fc9631cb94c22441dc427a9138277a37356a96370bb9366bc`.
Initramfs: `bb60e76cb40d65411dd98b5a50eab3c07d90e3eb62c6943d3d96e60de1b4843c`.
The preceding guest run's only smoke mismatch was its obsolete expected revision
19. The new smoke expects 20 and explicitly checks the Vulkan environment.
Local smoke helper unit tests pass; four unrelated shell-dependent release helper
tests could not run from native Python because bash was absent from its PATH.
The complete release helper suite passes in Linux CI.

The unsigned local launcher uses current application source from 7a15327 with
only its embedded candidate manifest and release variables changed. SHA256:
`9839bc43b483722c109eb8271b68410ee75a1543da592e3db7f176325b8a77de`.
Combined manifest: `9fb255dff78263bf4140063ff553cc00c3c9e8f8c5c66e63c284b31131a9a92e`.
It is served only at http://127.0.0.1:18080/v0.0.18-preview. No release was published.

The first update attempt mistakenly allowed discovery of the pre-existing
C:\WINQ-EMU runtime, which fails on the Unicode installation path. This was a
test configuration failure, not a failure of r15. The launcher recovered the
previous guest files; the persistence fixture still matched. After a clean
recovery boot, the three manually installed Vulkan environment files were removed.
The next launch explicitly excluded external runtime discovery and updated the
bundled runtime and guest using the authenticated candidate manifest.

PASS on the existing persistent installation:
- Boot reports compatibility `20:7.2.4-arch1-2` and guest-ready at 16:02:11.
- Packaged update recreates the removed environment files; UWSM inherits both
  settings. The common script hash is
  `537f3e7908b25564936d366485af8beabcf03526a7baf44a7eb447aa62235e43`.
- Both persistent test files retain their recorded hashes.
- Default systemd-launched mpv uses Vulkan, plays all 15 seconds and exits 0.
- Native Windows screenshot confirms visible moving test video at frame 218.
- Guest/runtime receipts identify the new manifest and runtime ZIP; no pending
  payload-update state remains after userspace readiness.
- Guest powers off cleanly at 16:03:30; no QEMU process remains.

Evidence under the acceptance root: r15-guest20-candidate.json,
guest20-verified-downloads.json, r15-full-native-handles.json,
r15-integrated-result.txt, r15-integrated-video.log, r15-integrated-video.png,
r15-update-recovery.txt and r15-integrated-shell.log. The final evidence hash
inventory is r15-integrated-evidence-hashes.json.

C: has 24,400,986,112 free bytes (22.72 GiB) after the integrated update.
No additional VM disk was created. The AMD playback blocker is closed on this
hardware with these exact artifacts; camera, portable lifecycle, the outstanding
full-feature implementations and hardware-specific gates remain unproven/open.
This is not an all-features release approval.

## v0.0.18-preview release preparation

After reviewing the remaining work, the user explicitly authorized publishing
the improvements as the latest preview and asked for release notes thanking
external contributors. Full-feature completion remains follow-up work.

The release reuses the exact runtime and guest payloads tested above, avoiding
a rebuild that would change the accepted artifacts. All ten upload files were
rehashed successfully. The release manifest normalizes the runtime's checksum
separator to two spaces (required by the source-pin validator); payload bytes
are unchanged. Its SHA256 is
`023908807e48848c972462c10dc01a31193e240140d466e72699953cd27c3346`.
The source pin and runtime lock now identify v0.0.18-preview.

Version resources were regenerated locally using `go-winres v0.3.3` with icon
resource ID 1 and the version fields read from `app/versioninfo.rc`, because the
laptop has no LLVM resource tools. Input JSON is retained as
`release18-resource.json` in the acceptance directory. The existing resource
tests validate both compiled numeric versions and both text versions.
The signed-package smoke and publication are recorded in the next checkpoint.

## Signed v0.0.18-preview package acceptance

PR #110 merged as `5e8e43bb9ce79176e5651fd19379808da8e9570f`.
Both the preparation CI (`34787698373`) and merged-master CI (`34787818447`)
passed all three required jobs. Azure signing-check run `34787825540` passed.
The downloaded signed launcher SHA256 is
`d9742f2d525a84ca2db080f6ff731ab4b124353c0a886ec42c4b016e93fc7187`.
Windows reports Valid Authenticode, signer Brandon South, and file version
v0.0.18-preview. The signed update metadata verifies with the launcher's trust key.

Using the existing disposable Unicode-path installation and explicitly selecting
the bundled runtime, the signed launcher passes:

- Authenticated delivery of the final release manifest and unchanged r15 runtime.
- GPU boot and userspace readiness at 17:50:28, with the update confirmed.
- Both persistent fixture hashes unchanged, with the packaged Vulkan environment
  inherited by the desktop session.
- Default Vulkan mpv playback through 100%, exit 0, 15.465 seconds, with a native
  screenshot at frame 280. One frame was reported dropped. The first scripted
  attempt ran before the Wayland desktop environment was ready and exited 2;
  the successful attempt explicitly waited for the graphical session.
- Clean poweroff at 17:51:35, normal relaunch at 17:51:54, and userspace readiness
  at 17:52:06 without another payload update.
- Persistent hashes still match on the second boot; clean poweroff at 17:52:48.

All eleven draft assets (ten payloads plus SHA256SUMS) also match GitHub's stored
SHA256 digests. The signed smoke used loopback URLs because draft assets require
authentication; the compiled defaults pin the public v0.0.18-preview URL.

Evidence under the acceptance root: `signed-v18-verification.json`,
`signed-v18-draft-asset-verification.json`, `signed-v18-smoke.txt` (early attempt),
`signed-v18-desktop-ready-smoke.txt`, `signed-v18-video.log`,
`signed-v18-video.png`, `signed-v18-relaunch.txt`, `signed-v18-shell.log`, and
`signed-v18-evidence-hashes.json`. No new VM disk was created. The publication
workflow is run `34788070204`, building the same merged application source.

## Publication complete

[v0.0.18-preview](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.18-preview)
was published at 2026-09-13 22:57:58 UTC and promoted to Latest. Release workflow
`34788070204` passed every required step, including Windows race tests and vet,
Azure signing, authenticated update metadata, and public tagged/Latest checks
for both `omacom/try-omarchy-windows` and the redirected `tsouth89` repository.
All 17 release assets are present. Published notes include the remaining preview
limitations and acknowledgments for external contributors and issue reporters.

An independent unauthenticated download from the public Latest URL on this
laptop passed launcher checksum, Valid Authenticode, file version, embedded
manifest pin, and Ed25519 update-signature verification. The published EXE SHA256
is `e7a274b57d3e85ebce987b1ee9dcf7377d924893127edae8d2f83685c8a37a9f`.
Its checksum differs from the signing-check EXE because the publish workflow
signs its own build of the same source. Evidence is
`public-v18-verification.json`, with downloaded public files under `public-v18`.

The test guest and loopback asset server are stopped. C: retains approximately
22.6 GiB free. Full-feature completion and the disclosed hardware/portable gates
remain follow-up work; this publication completes the user-authorized preview
release, not those remaining features.

## Post-release portable space and Windows publication fixes

The normal raw-installation portable path now inventories the backup allowlist,
hashes files, counts nonzero 64 KiB blocks, budgets QCOW2 metadata, copies and
verifies the payload files, and converts the original raw disk directly into
standalone QCOW2 in private staging. `qemu-img compare` verifies logical contents
before publication. It creates neither an intermediate backup archive nor a
restored raw disk. The source is retained. QCOW2-source copies keep their existing
factory-verification/materialization path and its larger space requirements.

Native Windows regressions using the released r15 qemu-img pass for a sparse
32 MiB source with only 4 MiB available above the production reserve, unchanged
payload bytes, excluded recovery data, full-disk rejection before output,
cancellation, missing conversion tool, pending update, and source preservation.
The complete portable-copy round trip remains covered by real conversion and
materialization tests.

The full suite exposed transient access-denied failures in move recovery and
snapshot rollback publication. The Windows publication wrapper now retries only
access/sharing/locking errors for at most 15 attempts, 200 ms apart. Recovery
remains available after cancellation. A native exclusive-handle test verifies
that publication succeeds when the temporary lock closes; permanent and exhausted
errors remain failures. The full native Go suite subsequently passed (27.738 s),
along with vet and focused portable/publication tests after the final change.

The actual stopped installation's read-only inventory found 286 entries and a
14,349,893,632-byte budget including the standard reserve, versus a
25,769,803,776-byte virtual disk. The acceptance harness additionally reserves
nine GiB, leaving ten GiB total headroom. Its large-copy attempt was rejected at
preflight and removed its empty private stage; no portable payload or disk was
created. Evidence: `portable-efficient-preflight.txt`,
`portable-efficient-create.txt`, and `portable_acceptance_test.go` under the
acceptance directory. The harness uses the source's loopback receipt identity.

C: unexpectedly fell from the release checkpoint's roughly 22.6 GiB free to
roughly 4.9 GiB before this copy could begin. The cause is not established. The
new source clone is only 8.7 MB, recent Go-cache files total about 157 MB, and
the active raw disk's allocated ranges total 6,892,027,904 bytes. Read-only scans
found no newly created large acceptance artifact explaining the change. The
guarded rejection is not a successful installed-guest portable lifecycle test.
No previously denied deletion was retried, and no Windows reboot was attempted.

### Direct QCOW2 copies and additional storage inventory

Direct portable creation now also handles standalone and factory-backed QCOW2.
QEMU receives explicit JSON format/backing descriptors, avoiding probing or
external backing chains. Factory hashes are verified before and after conversion.
QEMU measures required allocated clusters for the destination; logical comparison
and independent-image inspection remain publication gates. No intermediate raw
disk or archive is created for either supported input format.

Small real-r15-QEMU tests cover a 32 MiB factory-backed disk with a distinct 4 KiB
overlay write, a second-generation
standalone copy, unchanged source overlay bytes, rejected corrupted factory data,
and preserved original data after the original factory is changed. They run with
only 4 MiB available above the reserve and require no large acceptance copy.
The native Windows full Go suite passed (28.713 seconds); vet with
`-unsafeptr=false` also passed. The overlay-write fixture uses a QMP handshake
and quiet qemu-io output to avoid Windows monitor startup races.

The read-only `storage-inventory.py`/`storage-inventory.json` inventory counts
allocated bytes and deduplicates hard links. Acceptance artifacts occupy about
58.2 GB. The current disk, old `fresh` installation and retained rollback images
account for much of this. The cause of the preceding free-space change remains
unresolved.

Automatic review rejected a new cleanup command with only `blocked by policy`.
It did not execute. Do not retry these additional targets through another tool,
API, script or subset. All are relative to the September 13 acceptance root:

- `Move 世界/TryOmarchy/checkpoints/184b4dabfb0abc1c2fbbecb75d30bb61`
- `combined-r12-guest19`, `guest19-r2`
- `runtime-r7`, `runtime-r8`, `runtime-r9`, `runtime-r13`, `runtime-utf8-experiment`
- `runtime-artifacts`, `runtime-r8-artifacts`, `runtime-r9-artifacts`,
  `runtime-r11-artifacts`, `runtime-r12-artifacts`, `runtime-r13-artifacts`

The completed snapshot archive was rehashed before the proposed cleanup and still
matches `a8aada049e0590441473b90165872ca2ad61c487d81ae576baed95ba154dde88`.
No payload copy was retried. C: remains near 4.8 GiB free; physical portable
creation needs cleanup by the user or a separate suitable destination.
