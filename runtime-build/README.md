# Source-built WINQ-EMU runtime

This recipe builds Try Omarchy's Windows QEMU runtime from the exact QEMU and
virglrenderer fork commits in `sources.lock.json`. It produces:

- `winq-emu-alpha10-portable.zip`, a drop-in replacement for the current runtime
- `winq-emu-alpha10-source.zip`, the corresponding source and build recipe
- `SHA256SUMS`, hashes for both archives

The portable archive includes source provenance, the MSYS2 package inventory,
per-file hashes, and licenses. The Runtime workflow remains manual. Its r27
output is published as `runtime-v1-r27` and is pinned for the next app release;
r26 shipped in `v0.14.0`, r25 in `v0.13.0`, r24 in `v0.12.0` and `v0.12.1`, r23
in `v0.11.0`, r22 in `v0.10.0` and `v0.10.1`, r21 in `v0.9.0`, and r20c from
`v0.3.0` through `v0.8.0`. Future runtime
replacements stay test artifacts until they pass physical Windows validation.

To build in an MSYS2 UCRT64 shell with the packages in `packages.txt` installed:

```sh
runtime-build/build.sh runtime-output
```

Do not update `guest-build/runtime.lock.json` until the resulting runtime has
passed the Windows test checklist in `docs/RUNTIME-VALIDATION.md`.

The renderer image dispatch checks the exact host-allocation external format
capabilities before creating or querying an internal host variant, including
the maintenance4 requirements path. Rejected variants keep plain backing and
advertise its non-host-visible memory types. Disjoint images keep plain backing
because independent plane bindings cannot select one shared alternate handle;
requirements2 and maintenance4 preserve each plane's requirements. Explicit
external images keep their existing route. Unknown creation-info chains also
keep plain backing rather than assuming host-import support.

`0006-honor-venus-row-pitch-in-windows-gl-imports.patch` handles a Venus image
that OpenGL samples, such as a Vulkan app's window in the compositor. Its
memory is imported with `GL_EXT_memory_object`, which has no row pitch, so GL
lays the linear texture out with its own pitch instead of the image's. On
Intel's Windows drivers a window whose width is not a multiple of 64 pixels
reads skewed (#394). When the guest's row pitch is wider than the image,
the renderer views the memory as a texture one pitch wide and copies the visible
part into the resource's texture before anything reads it: draws, compute,
clears, blits, copies, uploads and readback. Anything OpenGL writes into it is
copied back into the memory straight afterwards, so Vulkan sees it too. It is on for Intel's Windows OpenGL driver; `TRYOMARCHY_PITCH_COPY=on`
or `off` overrides that. `test-image-pitch.py <patched-renderer-source>` covers
the decision, widths, bounds and refreshes against fake GL calls.

`0007-swap-bgra-in-windows-memory-object-imports.patch` corrects the colors of
those images (#400). A BGRA Venus image imported as a memory object is stored
as `GL_RGBA8` and never uploaded, so sampling it returned red and blue swapped.
The renderer already swapped R and B in the sampler view for Linux dma-buf
imports; the patch applies that to memory-object imports too, for views in a
BGRA format, and swaps the channels each swizzle entry selects. GL writes into
such an image (copies and uploads from the GL side) still store GL's own
order. `TRYOMARCHY_BGRA_SWAP=off` restores the old sampling. `test-bgra-swap.py
<patched-renderer-source>` covers the switch, BGRA and RGBA views, swizzles with
constants and the unchanged Linux path against fake sampler views.

`0008-compile-storage-buffer-shaders-as-glsl-430.patch` keeps OpenGL apps that
use shader storage buffers drawing, such as Ghostty (#383). The renderer
declares every storage buffer with the `std430` layout but wrote such shaders
as GLSL 1.30 to 1.50 with `GL_ARB_shader_storage_buffer_object`. AMD's Windows
driver rejects `std430` there ("not supported with this profile"), and the
failed compile puts the whole GL context in error, so the app's later draws
are dropped too and its window stays empty. When the host offers GLSL 4.30 in
a core profile, a shader with storage buffers is now written as GLSL 4.30.
Its inputs then drop the `invariant` qualifier that older versions need to
match an invariant output, because GLSL 4.30 rejects it on inputs.
`TRYOMARCHY_SSBO_GLSL430=off` restores the old version choice.
`test-ssbo-glsl430.py <patched-renderer-source>` covers the switch, the
version choice and invariant inputs against fake shader configs.

`0009-join-venus-workers-without-the-workers-lock.patch` keeps Omarchy from
freezing when a Vulkan app closes and another one starts (#407). On Windows
the render server runs inside QEMU, with a thread per Vulkan context. When an
app closed, the server waited for that context's thread to finish while
holding the lock on its list of contexts. Destroying the context waits for its
queue threads, and a queue thread that was just signaling a fence needed that
same lock. Nothing moved again, and the next Vulkan app froze the whole guest
because QEMU waits for the server to create its context. The server now takes
the context off the list, lets go of the lock, and then waits. A fence that
signals after that is dropped: QEMU has stopped reading the context's fences
by then. The context's socket used to be closed twice, by the server to wake
the thread and by the thread itself, and a thread still busy with a command
could then use a handle value Windows had already given to another socket.
Now the server sets a stop event instead. The thread's socket reads and
writes are overlapped and wait for either their completion or that event, so
a thread blocked in a read wakes at once and a busy one stops before its next
read. The server closes the socket once, after the thread has finished.
(`shutdown()` does not wake a blocked read on Windows while QEMU's end is
open.) `test-venus-worker-destroy.py <patched-renderer-source>` runs the
actual worker thread and teardown code with real threads and fake sockets,
under ThreadSanitizer on Linux: destroying, resetting or exiting while a
fence is being retired, a worker busy in a command, repeated destroys and a
worker whose peer already hung up, with each socket closed exactly once and
never used after. Under MSYS2 it also runs the worker's socket transfers on
the real loopback pair.

`test-image-capabilities.py <patched-renderer-source>` compiles the actual image
dispatch against mocked Vulkan entry points. It runs on Linux or MSYS2 without
a GPU or desktop and covers capability rejection, usable fallback masks,
dual backing, disjoint planes and bindings, and maintenance4. These checks do
not establish the cause of issue #276 or prove behavior on a Windows driver.

The r16 candidate adds independent startup SDL playback/recording selection via
`0013-select-sdl-audio-devices.patch`, with per-direction Windows-default fallback.
The build runs `test-sdl-audio.py` against the patched route function. On a
signed-in Windows desktop, run `smoke-audio.py <qemu.exe> --output <SDL-name>
--input <SDL-name>` to exercise real selected/default routes in a paused diskless
VM. See [audio behavior](../docs/AUDIO-DEVICES.md) and
[physical engineering acceptance](../docs/evidence/AUDIO-PARITY-2026-09-21.md).
The engineering build does not replace complete release packaging and acceptance.

The r17 candidate adds `0014-forward-windows-pinch.patch`: a dedicated virtio
touchpad and an opt-in Windows Precision Touchpad bridge. The build runs its
geometry/native-dispatch regression tests. `test-virtio-pinch.py` additionally
tests the actual guest ABI when `QEMU_PINCH_TEST_BINARY` names a qtest-capable
build; `smoke-memory.py --pinch` includes the device in the saved-RAM check.
The r18 recipe queries the exact Windows touchpad history size before reading
it, so a large first frame cannot be dropped by a fixed buffer. Set
`OMARCHY_PINCH_TRACE=1` during a physical test to log recognition and history
failures to QEMU stderr; it is off during normal use.
See [experimental pinch behavior and acceptance limits](../docs/PINCH-ZOOM.md).

The r19 recipe adds `0015-reclaim-free-guest-pages-on-whpx.patch`. It handles
virtio balloon free-page reports on WHPX by unmapping, decommitting, committing
and remapping private guest RAM. The recommit step guarantees zero-filled pages
when Linux reuses them. The launcher enables free-page reporting only when the
runtime's source provenance includes this patch, so older runtimes retain their
previous behavior. The [September 23 physical candidate record](../docs/evidence/FEATURE-GAPS-2026-09-23.md)
includes measured Windows memory return and reuse checks. The published
`v0.2.0` app pins r19.

The precise-scroll test recipe adds `0018-preserve-precise-sdl-scroll.patch`.
SDL 2.0.18 and newer supply fractional detents via `preciseX`/`preciseY`;
the shipped r20c inventory records SDL 2.32.10-1. SDL is selected by the MSYS2
package installation rather than pinned in `sources.lock.json`. Its Windows
[wheel message handler](https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/windows/SDL_windowsevents.c#L926)
divides `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` deltas by 120, preserving values below
one notch. Windows touchpad scroll settings are already reflected in those
messages; QEMU also handles SDL's explicit flipped-direction flag.

QEMU carries those deltas to the launcher's existing `virtio-tablet-pci` as
`REL_WHEEL_HI_RES`/`REL_HWHEEL_HI_RES`, with legacy events at 120-unit boundaries
for older Linux clients. The virtio mouse has the same wheel capabilities.
The dedicated pinch device still accepts only multitouch frames. PS/2 and USB
input retain accumulated whole-detent button events, and builds with older SDL
headers use the integer wheel fields. Each SDL window keeps its own fractional
remainder, cleared on focus loss, window destruction and VM state changes.

The build runs `test-sdl-scroll.py` and `test-virtio-scroll.py` against the actual
patched handlers. For diskless guest-ABI checks, run:

```sh
QEMU_SCROLL_TEST_BINARY=/path/to/qemu-system-x86_64 python3 runtime-build/test-virtio-scroll-abi.py
QEMU_PINCH_TEST_BINARY=/path/to/qemu-system-x86_64 python3 runtime-build/test-virtio-pinch.py
```

This remains a test runtime. Before pinning or releasing it, check smooth
vertical and horizontal precision-touchpad scrolling in Chromium and a
terminal on the Windows laptop, both Windows natural-scroll settings, plain
wheel-mouse notches, and pinch zoom followed by scrolling.

The r4 recipe enables libusb explicitly and includes its runtime DLL and license.
The USB host patch adds `auto-reconnect=off` for explicit attachment: the selected
bus/address must exist, vendor/product/port must still match, and opening the
device must succeed before QMP acknowledges it. Unplugging does not silently
claim a replacement device. The default preserves upstream auto-scan behavior.
CI verifies `usb-host`, `qemu-xhci` and the explicit-attachment property.

The r20 engineering recipe adds `0016-live-sdl-audio-routes.patch`. QEMU reads
private, atomically replaced `output` and `input` route files while streams
run, reopens only the changed direction, and falls back to the Windows default
if a selected endpoint disappears. The launcher writes the initial routes and
Settings can change them while a supported VM is running. Public `v0.3.0` ships
r20c as `runtime-v1-r20c`; public `v0.2.0` remains the prior r19 release. The
corrected r20b source-built archive passed the [available Windows laptop audio
checks](../docs/evidence/LIVE-AUDIO-R20-2026-09-23.md), including playback,
capture, route fallback and microphone gating. Guest PipeWire picker integration
is in guest patch 0091 and passed an isolated packaged boot. The signed and
public [v0.3.0 acceptance record](../docs/evidence/V030-SIGNED-CANDIDATE-2026-09-24.md)
records live Windows audio switching, persisted choices, the public update,
Windows boot and interrupted-update recovery. Two physical endpoints per
direction and hotplug remain untested. The route-file parser and SDL open/fallback
behavior are compiled by `test-sdl-audio.py` during the runtime build.

The r20c follow-up defers the initial SDL playback open until the guest starts
a stream. The r20b physical candidate kept a Realtek render session active
while the guest was idle after boot, even though subsequent idle periods closed
it. The signed v0.3.0 acceptance passed physical idle and first-playback checks
on the laptop.

The r5 recipe restores the Windows socket handle protection bit with an explicit
mask before closing the socket. The old zero-mask call left protection enabled
when the original flags were zero. A compiled source fixture covers all original
flag combinations. Saved-session tests require a complete socket stream and
never accept an idle timeout as end of data.

The r22 engineering candidate adds `0019-honor-whpx-cpu-model.patch` for the
launcher's `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes` CPU recovery mode.
WHP partition setup waits for the realized model, masks its instruction feature
bank and disables XSAVE. CPUID exits return QEMU's model, with live CR4 for
OSXSAVE and WHP's synthetic hypervisor leaves retained. Adding XSAVE, AVX, FMA,
F16C, XOP or FMA4 to qemu64 is rejected explicitly. Other CPU models, including
GPU mode's `host`, retain WHP's existing CPUID and native XSAVE layout. The
compiled setup fixture checks masks, ordering and fail-closed setup failures.
This restriction is a recovery control, not an explanation for issue #276.

`0020-allow-external-sdl-keyboard-hook.patch` recognizes
`OMARCHY_SDL_EXTERNAL_KEYBOARD_HOOK=1` on Windows. It disables SDL's keyboard
grab hook while preserving the mouse grab. The launcher sets it only when the
runtime provenance declares the patch, then chains host-owned Win events through
`CallNextHookEx`. Older runtimes retain the existing hook bypass. The SDL hint
fixture covers default behavior, the exact opt-out and hint priority.

Neither candidate changes `guest-build/runtime.lock.json` or publishes a release.

`0022-keep-sdl-capture-running-without-a-microphone.patch` skips Windows SDL capture
opens when no recording endpoint is enumerated. Without an endpoint it keeps
guest capture periods moving with format-aware silence paced by QEMU's virtual
clock. Empty inventories are checked once per second so reconnecting a
microphone can resume an active stream. Failed opens with listed endpoints wait
ten seconds after the attempt before retrying, including live route changes.
Stopped capture devices return to silence on the next get/read, after any
outstanding buffer has been consumed. Playback retains its existing behavior.

The build runs `test-sdl-capture.py <patched-qemu-directory>` against the actual
capture functions with mocked SDL endpoints and deterministic clocks. It covers
empty and failed inventories, silence pacing, repeated stream starts, failed
opens, active hotplug, unplug and reacquisition between buffer get/put, live route
failure backoff, idle gating, route fallback and unsigned PCM ring wrap. Windows
no-microphone menu behavior and physical hotplug remain to verify.
An endpoint disappearing between enumeration and open can still enter SDL's
WASAPI retry; this patch avoids the stable empty-inventory case.

The r23 engineering candidate adds
`0023-show-guest-cursors-in-sdl-windows.patch`. With an absolute pointer and
`show-cursor=off`, SDL hides the host cursor until the guest defines a sprite.
It then uses that sprite and hotspot as the native cursor. Guest hide requests,
transparent definitions and creation failures keep it hidden. Each console
retains its own pixels and visibility; window entry selects that console's
cursor, and leaving VM windows restores the native pointer. `show-cursor=on`
keeps the diagnostic host arrow override.

Sprites up to virtio's 64x64 limit scale with the displayed guest surface:
letterboxed surfaces use the smaller window scale, and GL scanouts use both
stretch factors. This uses shared SDL APIs and does not touch other displays.
The build runs `test-sdl-cursor.py` against the real cursor, grab, fullscreen,
window-event and window-destruction functions with mocked SDL. Windows motion,
click alignment, mixed DPI and physical multi-monitor behavior remain to verify
with the separate guest cursor fix and a built runtime.

`0024-raise-windows-fd-setsize-for-slirp.patch` builds QEMU with
`FD_SETSIZE=4096`. The Windows main loop polls slirp's host sockets with
`select()`, and Winsock's `fd_set` holds 64 sockets by default; `FD_SET`
silently ignores the rest. A guest with more than 64 open flows (a browser,
a package update, Tailscale) stops receiving replies on the extra sockets:
UDP answers and TCP data reach QEMU's sockets on the host but never reach the
guest, and forwarded ports stop accepting. Restarting the VM only helped until
the socket count grew again. The larger sets take about 96 KB of main-loop
stack, well inside QEMU's 2 MB thread stacks. `test-fd-setsize.py` binds 100
loopback sockets and checks that `select()` reports the last one ready with the
pinned size, and misses it with Winsock's default.

`0025-fail-fast-on-missing-sdl-audio-endpoints.patch` keeps a missing Windows
audio endpoint from freezing the VM. SDL's WASAPI backend retries a missing
endpoint for eight seconds inside `SDL_OpenAudioDevice`, which runs on QEMU's
main loop. Each guest sound, such as Hyprland 0.57's terminal bell, stopped
the display, input, QMP and slirp for that long, and the launcher reported the
VM as unresponsive (#384). Before opening, QEMU now checks SDL's inventory in
both directions and asks Windows directly whether a default endpoint exists.
A missing default, or an empty inventory, fails at once and is checked again on
the next route poll. A selected device that left the inventory goes straight to
the default, and a selected device that fails to open only falls back to the
default when Windows has one. Playback now backs off after a failed open like capture does,
across route polls and stream starts: ten seconds after an open that reached
SDL, one second after the quick checks found no endpoint. A successful open
clears it. A missing endpoint and an SDL open failure are each reported once
until that direction opens again.
On the reporter's kind of failure, a Windows VM without a default playback
device, the check answers in about 1 ms where SDL took 8 s.

`test-sdl-playback.py` compiles the playback start and route functions with
mocked endpoints and a fake clock: immediate reopen after a normal sound, no
repeat of a slow failed open across polls and repeated sounds, recovery, a
missing default, empty-inventory hotplug and a failed live route change.
`test-sdl-endpoint.py` compiles the Windows endpoint check against mocked COM
calls: the enumerator and interface GUIDs, the console role, present and
missing defaults, other errors, COM and loader failures, and released objects.
`test-sdl-audio.py` and `test-sdl-capture.py` cover the open preflight, the
fallback and single reports. An endpoint removed after the check can still enter SDL's
wait once; the backoff keeps it from repeating.

`0026-hold-sdl-windows-for-the-launcher.patch` lets the launcher hide a
display window before it is ever on screen (#413). While Omarchy boots, the
launcher hides QEMU's window with an empty window region and no frame.
Windows carries out those calls on QEMU's main thread, which goes on setting
up the machine for a second or more after it creates the window and handles
no window messages until then, so the window SDL had just shown stayed on
screen, an empty frame, for that time. With `OMARCHY_SDL_HOLD_WINDOWS_MS`
set, SDL creates a display window hidden and QEMU marks it with the
`TryOmarchy.HeldWindow` window property; the launcher shows it once it has
hidden it. A window still hidden that many milliseconds (at most 60 s) after
QEMU first handled window messages shows itself, so a launcher that never
shows it cannot leave it invisible. Consoles QEMU hides itself, and the
hidden window it creates to check for dma-buf support, are left alone.
`test-sdl-held-window.py` compiles the hold, the launcher's show, the
fallback and a window that cannot be marked against mocked SDL and Win32
calls, and checks where `sdl2.c` calls them.

`0027-make-full-screen-sdl-starts-resizable.patch` lets a window that starts
full screen leave it for a window that fits the screen. QEMU created such a
window without `SDL_WINDOW_RESIZABLE`, so after Ctrl+Alt+F neither the user
nor Windows could resize or maximize it. And while a window is full screen,
SDL takes the size QEMU gives it when the guest's display changes as the
size to restore later, so leaving full screen brought back the guest's
display size plus a frame, larger than the monitor's work area. A display
window is now always resizable. While it is full screen QEMU no longer sizes
it to the guest, so a window that started out as one gets back its own
size. One that started full screen gets three quarters of the usable area
of its display, centered, the first time it leaves full screen.
`test-sdl-fullscreen-window.py` runs the window flags, the guest's resize
and Ctrl+Alt+F against SDL's windowed size rules: windowed and full screen
starts, the launcher growing a full screen window, a second monitor and
missing usable bounds.

`0028-judge-sdl-hotkeys-by-their-key-events.patch` makes Ctrl+Alt+F and the
other Ctrl+Alt hotkeys work when they arrive together with other events.
QEMU handles SDL's event queue in one go, after SDL has read every pending
Windows message, so `SDL_GetModState()` already counts keys further down the
queue. A pointer entering the window just before Ctrl+Alt+F made the enter
event see Ctrl and Alt down and hold hotkeys back, and the keys went to the
guest instead; Ctrl and Alt let go in the same queue as F hid the hotkey. A
hotkey now uses the modifiers of its own key event, and enter and focus
events the modifiers as of the last key event handled. Ctrl and Alt held as a
window gets focus still hold hotkeys back until a key goes up.
`test-sdl-hotkey-modifiers.py` replays those queues through QEMU's key and
window event handlers against mocked SDL.

`0029-keep-the-window-size-after-full-screen.patch` keeps a window's size
when the guest catches up late after full screen. The guest takes the size
the window gives it a second or more later, so after a quick Ctrl+Alt+F
round trip it could still switch to the full screen size once the window had
left full screen, and QEMU sized the window to it: the monitor's size plus a
frame. SDL also kept that as the windowed size for later rounds. The sizes a
window has while full screen are now remembered from SDL's resize events,
and after leaving full screen the window does not follow the guest to one of
them unless it is the size the window asks for now, until the user asks for
the guest's size with Ctrl+Alt+0 or the window goes full screen again. A busy
guest can be a round behind and show the last round's windowed size before
this round's full screen size, so the sizes the guest shows do not end it.
Ctrl+Alt+F switches only the window it is pressed in while `gui_fullscreen`
is shared by every window, so it goes by that window's own SDL flag, and each
window remembers its own full screen size when it enters and leaves full
screen. A window created again starts clean, and a display without ui-info is
never asked for a size, so it follows the guest. `test-sdl-fullscreen-window.py`
runs a guest that switches late, for windowed and full screen starts and for
two windows, through QEMU's own `sdl2_window_create`.

`0030-show-held-windows-with-their-first-frame.patch` shows a held window
together with its first frame. Nothing QEMU presents reaches a window whose
region the launcher has emptied, so when the curtain lifted the window showed
what its surface held from before until QEMU's next frame: black, and on some
GPUs a white block where the window was first created. A held window is now
also cloaked with `DWMWA_CLOAK`; DWM goes on composing it, so frames reach it.
Once it is shown with a region that is not empty, QEMU paints it black, draws
a frame, and uncloaks it once DWM's composed frame count has moved on since a
frame at its current size was presented. QEMU only reads that count and never
waits on DWM, so a paused or slow compositor cannot stall its main loop. It is
uncloaked anyway one second after it came on screen
without such a frame, ten minutes after it was cloaked, on Ctrl+Alt+F and
when it is destroyed; if DWM refuses the cloak the window behaves as before.
`test-sdl-held-window.py` also runs the cloak against mocked Win32 and DWM
calls: frames drawn hidden, with an empty region, at another size or before
the window got its frame back, a region
emptied and restored between two rounds, a resize after the frame, paused
composition and an unreadable frame count, both time limits, the old self show, every exit and refused
calls, and checks that every present reports its size.
