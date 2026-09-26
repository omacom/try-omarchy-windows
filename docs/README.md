# Try Omarchy for Windows guides

Start with the [quick start](../README.md#quick-start). For supported hardware and current limits, read [App compatibility](COMPATIBILITY.md).

## Using Omarchy

| Task | Guide |
| --- | --- |
| Learn the keyboard shortcuts | [Essential keys](../README.md#essential-keys) |
| Change CPU, RAM, display, audio, or camera settings | [Settings](../README.md#settings), [Desktop controls](DESKTOP-CONTROLS.md), [Audio devices](AUDIO-DEVICES.md) |
| Share a folder or connect over SSH | [Shared-folder settings](../README.md#settings), [SSH and port forwarding](../README.md#ssh-and-port-forwarding) |
| Update an existing Linux guest | [Guest upgrades](GUEST-UPGRADES.md) |
| Back up, restore, or reset a guest | [Backup and recovery](BACKUP.md) |
| Move an installation to another drive | [Moving an installation](MOVING.md) |
| Export Omarchy configuration | [Configuration migration](MIGRATION.md) |
| Launch approved Windows apps | [Windows app bridge](WINDOWS-APP-BRIDGE.md) |
| Troubleshoot slow or unsupported graphics | [Performance](PERFORMANCE.md), [App compatibility](COMPATIBILITY.md) |
| Report a problem | [Diagnostics and reports](../README.md#reporting-a-problem) |

## Experimental features

These guides describe their own limits and validation status. They are not a promise of support on every PC.

- [Portable USB installations](PORTABLE_USB.md)
- [Trackpad pinch](PINCH-ZOOM.md)
- [Nested virtualization](NESTED-VIRTUALIZATION.md)
- [GPU application profiles](GPU-APPLICATIONS.md)

## Development and testing

- [Contributing](../CONTRIBUTING.md): repository layout, build, and automated checks
- [Windows testing](TESTING.md): physical checks and what to include in a report
- [Mac feature comparison](MAC-PARITY.md): shipped behavior, experiments, and remaining work
- [Release process](RELEASING.md): signed candidates, acceptance, and publication
- [Guest build](../guest-build/README.md) and [runtime build](../runtime-build/README.md)

The [evidence directory](evidence/) records specific machines, versions, and test sessions. Older evidence does not establish acceptance of a newer release.
