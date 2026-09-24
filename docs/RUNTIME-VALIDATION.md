# Source-built runtime validation

September 13 physical laptop results are recorded in
[WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md](evidence/WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md).
That round found r7 shared-folder Unicode/timestamp failures and Unicode
installation-path failures. The r8 sharing correction passes on the laptop;
r9 path correction passes restored Unicode-path boot. Multi-display failures
produced r10/r11/r12 corrections; r12 three-output power cycling and secondary
rendering pass physical checks, including a 61-sample, 3,613-second endurance run,
three-output CPU rendering, graceful close confirmation and induced GPU-to-CPU
fallback. Physical keyboard/focus acceptance remains open. Default mpv Vulkan
playback failed through r13. The complete r15 runtime and compatibility-20 guest
now pass default Vulkan playback, visible output, persistence and shutdown on
this AMD laptop. They include host-imported memory backing plus guest-side
presentation and driver-lifetime workarounds. The public pin remained unchanged
while the next signed candidate was prepared. See the laptop report for the
exact artifact hashes and the distinction between engineering and final builds.

The published runtime already uses our source-built `winq-emu-alpha10-source-r3`
archives. On September 12, both public v0.0.9-preview archives were downloaded
and verified against `guest-build/runtime.lock.json`; the bundled source lock
matches the current recipe. The Alpha 10 filenames are retained for compatibility.

The Runtime workflow produces replacement test artifacts. Keep the current pin
until a replacement passes the applicable checks below on available physical
Windows hardware. Record untested configurations rather than treating them as
failures.

- `qemu-system-x86_64.exe --version` reports QEMU 11.0.0.
- `qemu-system-x86_64.exe -accel help` lists WHPX.
- Try Omarchy reaches the desktop using the rebuilt ZIP.
- Venus Vulkan starts on a supported GPU, including the existing GPU probe.
- CPU rendering still takes over when the GPU probe is forced to fail.
- Keyboard input, scoped Windows key handling, clipboard, audio, and sharing work.
- The host and guest cursors stay aligned during fast movement and fullscreen.
- Windowed, fullscreen, guest reboot, guest poweroff, and relaunch all work.
- The runtime archive extracts cleanly on a fresh machine without MSYS2 installed.
- Task Manager shows no unexpected console window or extra launcher process.
- After the desktop settles, QEMU's Task Manager CPU use falls materially below
  its active-animation level and does not pin one logical processor. Animation
  and video remain smooth when display activity resumes.
- When available, test a host that refuses nested virtualization (such as
  some Intel Core Ultra laptops or hosts with the full Hyper-V feature set).
  QEMU starts and
  `qemu-stderr.log` shows the "nested virtualization unavailable" warning
  instead of `Failed to enable nested virtualization` (issue #19).

Before changing the public pin, record the launcher version, runtime hash,
Windows build, and driver version using [TESTING.md](TESTING.md). Expand AMD,
Intel, NVIDIA, and Windows-version coverage as machines and bug reports become
available; this broader matrix is not a prerequisite for publication.

For the unpublished v1.0.0 candidate, the exact r18 runtime was available for
physical acceptance on the AMD Windows 11 laptop; the Intel/NVIDIA PC remained
booted into Omarchy. The [r18 laptop record](evidence/PINCH-R18-PHYSICAL-2026-09-22.md)
records its package hashes, GPU boot, and touchpad test. The earlier
[Intel/NVIDIA test](evidence/RESOURCE-PROFILES-INTEL-NVIDIA-2026-09-20.md)
used the v0.0.20-preview runtime: VirGL OpenGL worked, while Venus Vulkan
initialization failed. This remains a documented compatibility result, not
evidence that r18's NVIDIA Vulkan path passes. Follow up when that hardware is
available or a user reports the same issue on the published runtime.

The public `v0.2.0` release uses the r19 runtime, which adds WHPX free-page
reclamation. On September 23 the AMD laptop booted its full GPU desktop with
this runtime, and a separate 4 GiB headless guest returned about 797 MiB to
Windows after a 768 MiB touch/free cycle without shrinking the guest's
configured capacity. The exact runtime archive and acceptance commands are in
[FEATURE-GAPS-2026-09-23.md](evidence/FEATURE-GAPS-2026-09-23.md). The
[signed v0.2.0 candidate](evidence/V020-SIGNED-CANDIDATE-2026-09-23.md) booted
the bundled runtime on the same laptop.

The r20c runtime is published as `runtime-v1-r20c` and pinned for the v0.3.0
candidate. Its signed packaged candidate passed Windows boot, saved-choice
persistence across guest restart, and idle playback-device release on the AMD
laptop. Public `v0.2.0` remains on r19 until v0.3.0 publishes. See the
[live audio record](evidence/LIVE-AUDIO-R20-2026-09-23.md).
