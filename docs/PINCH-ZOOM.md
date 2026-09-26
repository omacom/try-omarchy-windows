# Windows trackpad pinch

The runtime adds a dedicated `virtio-pinch-pci` touchpad and a Windows Precision
Touchpad bridge, first in r17 and bundled since r20c. It sends multitouch
contacts to Linux; it does not substitute zoom keyboard shortcuts. The ordinary
Virtio Tablet keeps pointer movement, clicks and scrolling.

The launcher turns pinch on by default when the runtime carries the pinch patch,
the VM uses GPU mode with one display, and the installed guest image lists
`virtio-pinch-pci` under `runtime.optionalDevices` in `build-spec.json`. Guest
patch `0095` adds that declaration; images without it, including `v0.3.0` and
older payloads restored from a checkpoint, never receive the device. There is
no setting. `-disable-pinch` keeps ordinary Windows two-finger input, and
`-experimental-pinch` still forces the device on for a guest configured by hand.
Each launch logs the decision as `touchpad pinch forwarding` in `shell.log`.

## Host behavior

The bridge dynamically resolves Windows 11's touchpad and Interaction Context
APIs, including the documented ordinal for
[`ProcessPointerFramesInteractionContext2`](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/processpointerframesinteractioncontext2).
An older or unsupported host retains normal SDL input. Windows 10 was not tested.
Interaction Context performs recognition using Windows' touchpad settings.

Pure pans stay with Windows/SDL. Only recognized scaling creates virtual contacts.
Two symmetric diagonal contacts keep the center and angle fixed; the diagonal
avoids libinput's scroll heuristic for slowly converging, horizontally aligned
fingers. Scale is bounded to 0.125–3.5 times the starting spacing per gesture.
Lift and begin another gesture to continue beyond those bounds.

Focus loss, cancellation, destruction, pause/resume and VM reset clear gesture
state. Resume emits releases because QEMU discards normal input while paused.
Windows Interaction Context reset calls are deferred to the window thread.
Updates after cancellation cannot resume old contacts without a new begin.
Motion callbacks are coalesced once per SDL poll while preserving contact
transitions, preventing batched Windows history from appearing as touch jumps.

Registration changes how Windows delivers two-finger input, so it only happens
when pinch is on. Microsoft's
[registration contract](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/registertouchpadcapable)
describes the default-window-procedure fallback used for ordinary scrolling.

## Guest configuration

Guest recipe patch `0083` installs `/usr/share/try-omarchy/pinch-input.lua` and
loads it for new factory users. A complete factory image was rebuilt after the
reviewed eight-package lock refresh in patch `0084`. Its fresh Windows GPU boot
provisioned the override automatically and passed Hyprland configuration checks.

Guest recipe patch `0093` extends this to persistent guests. Compatibility
revision 34 delivers the rules file to disks created by older releases, and
the boot-time catch-up appends a guarded loader to each user's
`~/.config/hypr/input.lua` once. The original is kept as
`input.lua.before-try-omarchy-pinch`. The loader only runs when the rules file
exists, so a configuration exported to a real Omarchy install keeps working.
The unguarded line from `0083` images is replaced. A symlinked `input.lua`, one
that ends in a top-level `return`, or one that already configures
`qemu-virtio-pinch-touchpad` is left alone.

A udev rule sets `LIBINPUT_IGNORE_DEVICE` on the pinch device until
`try-omarchy-pinch-ready` confirms at each boot that every desktop user's
Hyprland configuration loads the rules. A guest that could not be migrated
keeps ordinary pointer, click and scroll input and does not receive synthetic
contacts. The reason is in `/run/try-omarchy/pinch-gestures` and in
`journalctl -u try-omarchy-pinch-ready`. The published `v0.3.0` guest predates
`0093`. Forcing pinch on for such a guest with `-experimental-pinch` still
needs this device-only override in `~/.config/hypr/input.lua`, with a backup
first:

```lua
hl.device({
  name = "qemu-virtio-pinch-touchpad",
  tap_to_click = false,
  disable_while_typing = false,
})
```

Reload Hyprland and check `hyprctl configerrors`. This prevents synthetic contacts
from becoming tap clicks and keeps keyboard palm rejection from suppressing them.
Do not force the bridge on for other pre-`0093` guests without it.

## Acceptance tools and limits

- `runtime-build/test-windows-pinch.py` compiles the exact geometry and native
  output callback from the patch, including pan versus pinch, invalid input,
  cancellation and pause/resume behavior.
- `runtime-build/test-virtio-pinch.py`, with `QEMU_PINCH_TEST_BINARY` set, exercises
  the actual PCI capabilities, virtqueue contacts/releases and separate button
  routing in an isolated diskless QEMU.
- `runtime-build/inject-pinch-test.c` is a separately built, bounded test helper.
  It requires an explicit QEMU PID and a unique visible foreground SDL window.
  Modes are `out`, `in`, `pan`, `cancel` and `focus`; it restores the cursor and
  destroys its synthetic device afterward. It is not shipped in the launcher.
- `scripts/guest/check-pinch-events.py` observes only the dedicated device through
  libinput. `--expect scroll` additionally observes the Virtio Tablet. It reports
  recognized pinch begin/update/end and rejects clicks and libinput errors; it does not grab
  input or open a keyboard device.

Synthetic Windows-to-libinput tests passed on the laptop, including both pinch
directions, scrolling, cancellation, focus loss and pause/resume, with no libinput
errors on the final candidate. Coalescing fixed the earlier touch-jump warnings.
A fresh guest also passed native Wayland Chromium zoom: synthetic outward pinch
changed the measured viewport scale from 1.00 to 1.66, and inward pinch restored
1.00. On September 22, physical fingers zoomed Chromium and normal scrolling
still worked, but starting the r17 pinch took too much effort. The r18 recipe
queries Windows pointer history size before reading it. On the same laptop, the
user reported that pinch was easier to start, could zoom back, and preserved
two-finger scrolling. Firefox, mixed DPI, fullscreen, and broader hardware
remain acceptance work. On September 26, physical pinch zoom and Ctrl+Alt+End
passed on the laptop guest migrated by `0093`.

See [the September 21 continuation record](evidence/PINCH-NESTED-2026-09-21.md).
