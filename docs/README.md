# Try Omarchy for Windows guides

Start with the [quick start](../README.md#try-it). For supported hardware and current limits, read [App compatibility](COMPATIBILITY.md).

## Using Omarchy

| Task | Guide |
| --- | --- |
| Learn the keyboard shortcuts | [Essential keys](USER-GUIDE.md#essential-keys) |
| Change CPU, RAM, display, audio, or camera settings | [Settings](USER-GUIDE.md#settings), [Desktop controls](DESKTOP-CONTROLS.md), [Audio devices](AUDIO-DEVICES.md) |
| Share a folder or connect over SSH | [Shared-folder settings](USER-GUIDE.md#settings), [SSH and port forwarding](USER-GUIDE.md#ssh-and-port-forwarding) |
| Update an existing Linux guest | [Guest upgrades](GUEST-UPGRADES.md) |
| Back up, restore, or reset a guest | [Backup and recovery](BACKUP.md) |
| Move an installation to another drive | [Moving an installation](MOVING.md) |
| Install Omarchy for real and bring your trial along | [Moving to a real install](MIGRATION.md) |
| Launch approved Windows apps | [Windows app bridge](WINDOWS-APP-BRIDGE.md) |
| Open links from Windows in Omarchy | [Windows link bridge](WINDOWS-LINK-BRIDGE.md) |
| Open shared files in Windows apps | [Windows file bridge](WINDOWS-FILE-BRIDGE.md) |
| Approve `sudo` with Windows Hello | [Windows Hello for guest sudo](WINDOWS-HELLO.md) |
| Zoom with a touchpad pinch | [Trackpad pinch](PINCH-ZOOM.md) |
| Troubleshoot slow or unsupported graphics | [Performance](PERFORMANCE.md), [App compatibility](COMPATIBILITY.md) |
| Report a problem | [Diagnostics and reports](USER-GUIDE.md#reporting-a-problem) |

## Experimental features

These guides describe their own limits and validation status. They are not a promise of support on every PC.

- [Portable USB installations](PORTABLE_USB.md)
- [Nested virtualization](NESTED-VIRTUALIZATION.md)
- [GPU application profiles](GPU-APPLICATIONS.md)
- [Windows NVIDIA bridge shader diagnostic](LIVE-GPU-SHADER-TRIANGLE.md): verified guest drawing; desktop integration remains unfinished
- [Guest shared texture and Windows resource import](LIVE-GPU-SHARED-RESOURCE.md): presentation prerequisite verified separately from drawing
- [Windows GPU consumption of guest textures](LIVE-GPU-SHARED-TEXTURE-CONSUMPTION.md): fenced handoff and native GPU copies verified separately from presentation
- [Native DXGI presentation of guest shader frames](LIVE-GPU-PRESENTATION.md): backbuffer bytes and presentation statistics verified; import stability and Omarchy display integration unfinished
- [Typed textures with Windows-generated metadata](LIVE-GPU-NATIVE-METADATA.md): three repeated GPU consumption runs pass; two newer presentation-statistics failures preserved

## Development and testing

- [Contributing](../CONTRIBUTING.md): repository layout, build, and automated checks
- [Windows testing](TESTING.md): physical checks and what to include in a report
- [Mac feature comparison](MAC-PARITY.md): shipped behavior, experiments, and remaining work
- [Release process](RELEASING.md): signed candidates, acceptance, and publication
- [Guest build](../guest-build/README.md) and [runtime build](../runtime-build/README.md)

The [evidence directory](evidence/) records specific machines, versions, and test sessions. Older evidence does not establish acceptance of a newer release.
