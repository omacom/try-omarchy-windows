# USB devices

USB passthrough is experimental and depends on the device's Windows driver.
Try Omarchy does not replace drivers.

## USB selection before launch

Settings > Devices > **USB device for next start...** remembers one device at
its current USB port. Saving the choice asks before granting startup access.
Windows applications lose access while Omarchy owns the device. Eject mounted
storage before switching it. **Don't attach** disables startup attachment while
retaining the saved choice.

The runtime lists devices without starting a guest or taking a device away from
Windows. A missing choice stays visible. Moving it to another port requires a
new selection. Missing, busy or unsupported devices do not prevent Omarchy from
starting. The launcher tries once per VM boot and does not reclaim an unplugged
and reconnected device automatically. Live Attach and Release remain available
from the tray's USB devices menu. No driver is installed by this flow.
