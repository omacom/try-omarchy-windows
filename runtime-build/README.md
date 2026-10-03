# Source-built WINQ-EMU runtime

This recipe builds Try Omarchy's Windows QEMU runtime from the exact QEMU and
virglrenderer fork commits in `sources.lock.json`. It produces:

- `winq-emu-alpha10-portable.zip`, a drop-in replacement for the current runtime
- `winq-emu-alpha10-source.zip`, the corresponding source and build recipe
- `SHA256SUMS`, hashes for both archives

The portable archive includes source provenance, the MSYS2 package inventory,
per-file hashes, and licenses. The Runtime workflow remains manual. Its r20c
output is published as `runtime-v1-r20c` and is pinned by public `v0.3.0`;
future runtime replacements stay test artifacts until they pass physical
Windows validation.

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
