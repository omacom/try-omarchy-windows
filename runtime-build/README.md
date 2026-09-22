# Source-built WINQ-EMU runtime

This recipe builds Try Omarchy's Windows QEMU runtime from the exact QEMU and
virglrenderer fork commits in `sources.lock.json`. It produces:

- `winq-emu-alpha10-portable.zip`, a drop-in replacement for the current runtime
- `winq-emu-alpha10-source.zip`, the corresponding source and build recipe
- `SHA256SUMS`, hashes for both archives

The portable archive includes source provenance, the MSYS2 package inventory,
per-file hashes, and licenses. The Runtime workflow is manual while the output
is being compared with the currently shipped alpha 10 archive on real Windows
hardware.

To build in an MSYS2 UCRT64 shell with the packages in `packages.txt` installed:

```sh
runtime-build/build.sh runtime-output
```

Do not update `guest-build/runtime.lock.json` until the resulting runtime has
passed the Windows test checklist in `docs/RUNTIME-VALIDATION.md`.

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

The r4 recipe enables libusb explicitly and includes its runtime DLL and license.
The USB host patch adds `auto-reconnect=off` for explicit attachment: the selected
bus/address must exist, vendor/product/port must still match, and opening the
device must succeed before QMP acknowledges it. Unplugging does not silently
claim a replacement device. The default preserves upstream auto-scan behavior.
CI verifies `usb-host`, `qemu-xhci` and the explicit-attachment property.

The r5 recipe restores the Windows socket handle protection bit with an explicit
mask before closing the socket. The old zero-mask call left protection enabled
when the original flags were zero. A compiled source fixture covers all original
flag combinations. Saved-session tests require a complete socket stream and
never accept an idle timeout as end of data.
