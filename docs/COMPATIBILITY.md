# App compatibility

Try Omarchy runs the full x86_64 Arch Linux environment used by Omarchy. It is not a reduced web demo or a compatibility layer.

## What works

- Omarchy desktop apps, themes, menus, keybindings, and screensavers
- Arch packages installed with `pacman`
- AUR packages installed with the preinstalled `yay` helper and the included `base-devel` toolchain
- Graphical Linux applications, including Visual Studio Code
- Web browsing and outbound networking through the Windows connection
- Audio, two-way text and image clipboard sharing, and persistent files inside the guest
- Host-folder sharing through the recommended `Omarchy Shared` folder or
  `-share <folder>` when the WINQ-EMU runtime is available, including CPU rendering

## Current limits

- 64-bit Windows 10 or 11 with hardware virtualization is required.
- ARM64 Windows PCs (Snapdragon and similar) are not supported: the launcher, runtime, and guest image are all x86_64, and setup stops with an explanation instead of blaming virtualization settings.
- GPU acceleration depends on the patched WINQ-EMU runtime and compatible Windows graphics drivers. Try Omarchy falls back to CPU rendering when that path is unavailable.
- USB devices, host webcams, and arbitrary PCI devices are not passed through.
- Networking uses QEMU NAT. Services inside the guest are not exposed to the Windows network automatically.
- Host-folder sharing is not available with an external stock QEMU fallback.
- Text and image clipboard sharing work in both directions (images travel as
  PNG, up to 16 MiB). File clipboard and drag and drop are not implemented
  yet; use the shared folder for files.
- The launcher boots its pinned kernel and initramfs from the release image, and the guest's pacman configuration holds the `linux` package so `pacman -Syu` and `omarchy-update` leave it alone. Kernel updates arrive with guest-image updates, which also carry the matching modules onto existing disks. Forcing a different kernel package into the guest leaves it out of sync with those boot files.
- Configuration export and restore are available through `try-omarchy-export`; see [the migration guide](MIGRATION.md). Development builds also support [stopped-VM backup and restore](BACKUP.md) from Settings or command-line options. Reset can retain the old disk and offer a full backup first. Snapshots are not available yet.

Compatibility varies with Windows, CPU, GPU, and driver combinations. When reporting a problem, include those details and whether Try Omarchy selected GPU or CPU rendering.
