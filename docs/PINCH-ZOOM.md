# Experimental Windows trackpad pinch

The r17 engineering runtime adds a dedicated `virtio-pinch-pci` touchpad and an
opt-in Windows Precision Touchpad bridge. It sends multitouch contacts to Linux;
it does not substitute zoom keyboard shortcuts. The ordinary Virtio Tablet keeps
pointer movement, clicks and scrolling.

Enable only for acceptance testing with `-experimental-pinch` and `-winq` pointing
to the supporting runtime. The launcher requires GPU mode and one display, and
removes inherited enablement when the flag is absent. The public v20 runtime is
unchanged. There is no default-on UI checkbox or claim of general availability.

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

Registration changes how Windows delivers two-finger input, so it is deliberately
restricted to the experimental path. Microsoft's
[registration contract](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/registertouchpadcapable)
describes the default-window-procedure fallback used for ordinary scrolling.

## Guest configuration

Guest recipe patch `0083` installs `/usr/share/try-omarchy/pinch-input.lua` and
loads it for new factory users. A complete factory image was rebuilt after the
reviewed eight-package lock refresh in patch `0084`. Its fresh Windows GPU boot
provisioned the override automatically and passed Hyprland configuration checks.
Existing persistent guests need this device-only override in
`~/.config/hypr/input.lua`, with a backup of that file first:

```lua
hl.device({
  name = "qemu-virtio-pinch-touchpad",
  tap_to_click = false,
  disable_while_typing = false,
})
```

Reload Hyprland and check `hyprctl configerrors`. This prevents synthetic contacts
from becoming tap clicks and keeps keyboard palm rejection from suppressing them.
The test laptop's existing guest has this override and a pre-change backup.
Do not enable the experimental bridge on other existing guests without it.

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
still worked, but starting the pinch took too much effort. The r18 recipe now
uses a size query before reading Windows pointer history; physical retesting is
required to see whether that improves the start. Firefox, mixed DPI, fullscreen,
and broader hardware also remain acceptance work. Pinch remains experimental.

See [the September 21 continuation record](evidence/PINCH-NESTED-2026-09-21.md).
