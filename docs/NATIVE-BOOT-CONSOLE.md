# Boot display for the native GPU session

**November 2, 2026:** an explicit graphics-only boot-console mode is implemented.
CPU ownership, queued-redraw and real-launcher argument controls pass, as do both
strict Windows host builds. A rebuilt QEMU now passes seven stopped, windowless
console controls locally. A subsequent normal session now submits its first
61 GPU frames across the transition; the captured scene is incomplete and
shutdown fails. [Actual transition and tested limits](NORMAL-SESSION-FIRST-GPU-FRAMES.md).
Counts remain **backend 4/8, desktop 0/4, scene 3/3**.

## Why this is needed

The existing native diagnostic profile removes the VGA device. That leaves the
normal tty1 owner-provisioning prompt without a visible boot display. Simply
adding VGA alongside the relay would create a second graphical console and SDL
window. Normal startup needs setup and GPU rendering in the same window.

The new `BootDisplay::StandardVga` composer option replaces only the launcher's
graphics device with `VGA,id=wddm-boot-display` and binds the relay through
`boot-display=wddm-boot-display`. It preserves the original SDL options, keyboard,
tablet, QMP roles and every other original argument. The existing default
composition remains unchanged. The owned host now exposes an explicit
`--driver-boot-console` selection requiring its persistent original profile and
lifetime binding. It is not enabled in the released launcher.
[Matched normal-bootstrap candidate and controls](NORMAL-SESSION-BOOTSTRAP.md).

## Implemented handoff

The optional relay property borrows the primary standard VGA console instead of
creating another console. It requires one graphical console and synchronous,
non-GL VGA callbacks. A second owner, unsupported callback set, missing display
or wrong device is rejected. A reversible callback proxy delegates VGA redraws,
invalidations and text updates while setup is active.

Host attachment imports and initializes the native resources without replacing
the boot surface. The first accepted native-frame command suppresses VGA redraws,
resizes the same console and verifies the root GL adapter. Native rendering keeps
the existing GPU copy, producer/consumer fences and SDL scanout route. Entering
scanout discards a queued boot-surface redraw so it cannot overwrite that frame.
These operations do not alter SDL event handling or Windows input routing.

Successful native teardown must retire the GPU work, disable scanout, release
imports and destroy the import context before VGA callbacks resume. Failed
retirement keeps the native ownership state. Unrealize restores a borrowed
console's callbacks rather than closing that console, and never overwrites an
unexpected successor. Native-device hot unplug remains disabled.

## Observed controls and limits

- The production ownership helper passes strict C CPU controls, including 10,000
  suppressed VGA redraw/invalidations/text updates and successor-identity checks.
  QEMU callbacks are stand-ins; this is not a rendered-frame test.
- Both strict Windows host configurations pass. The graphics argument test
  passes 53 checks. The real Go launcher exports five profiles: two supported
  cursor preferences and three rejected profiles. Both graphics modes preserve
  the actual exported arguments; all 434 current launcher source files remain unchanged.
- Seven SDL controls pass: input, cursor, keyboard, existing scanout teardown,
  native queued-redraw removal, original-source default compatibility, and an
  original-source negative control rejected specifically by the new redraw
  assertion. Only the common scanout test fixture gains a field and an explicit
  lab assertion option; its ordinary upstream test remains compatible.
- The compact graphics patch applies to the exact corresponding source of the
  previously built, locked QEMU candidate. Its recorded official input files
  remain byte-identical. No VM, renderer window, GPU work or host input was used
  in these local checks.

Seven additional real-QEMU controls now pass locally with stopped CPUs, no disk
and `-display none`. They check realization, rejected commands and complete
unchanged boot pixels. The relay reports borrowed ownership at console zero;
explicit VGA and default-console screenshots read the same complete surface.
The existing socket-reconnect controls also pass on this exact binary. No SDL
window or GPU context is opened, so these tests do not establish a displayed
GPU transition, normal guest startup or physical window behavior.

The first CI test used unsupported name lookup for a busless relay. Its failures
remain retained; the corrected test uses a PCI display for rejection and QMP's
documented default-console selection for the original relay. The artifact's new
helper header also differs from Git only by Windows checkout line endings. Its
exact built bytes pass the CPU ownership controls, and a specific Git attribute
now enforces LF for future builds. The original compiled-source hashes remain
recorded rather than replaced by normalized hashes.

Next integration must use the rebuilt QEMU and matched normal guest bundle,
keep the boot-only VGA device out of the native compositor's renderer selection,
and verify the real provisioning/login flow and first complete GPU desktop.
Application environment propagation, sustained interactive use, cleanup/recovery,
broader CUDA/OpenGL/Vulkan/video, DLSS, hardware ray tracing and the requested
**at least 93% matched-native performance** remain unverified.

[Initial source evidence](evidence/native-boot-console-2026-11-02.json) ·
[Compiled QEMU controls](evidence/native-boot-console-runtime-2026-11-02.json) ·
[Native renderer isolation](NATIVE-SESSION-RENDERER-SELECTION.md) ·
[Root activation service](ROOT-SESSION-SERVICE.md) ·
[Daily-use acceptance](DAILY-USE-READINESS.md).
