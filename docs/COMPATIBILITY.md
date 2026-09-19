# App compatibility

Try Omarchy runs the full x86_64 Arch Linux environment used by Omarchy. It is not a reduced web demo or a compatibility layer.

## What works

- Omarchy desktop apps, themes, menus, keybindings, and screensavers
- Arch packages installed with `pacman`
- AUR packages installed with the preinstalled `yay` helper and the included `base-devel` toolchain
- Graphical Linux applications, including Visual Studio Code
- Web browsing and outbound networking through the Windows connection
- Audio, two-way text, image, file and folder clipboard sharing, and persistent files inside the guest
- Host-folder sharing through the recommended `Omarchy Shared` folder or
  `-share <folder>` when the WINQ-EMU runtime is available, including CPU rendering

## Current limits

- 64-bit Windows 10 or 11 with hardware virtualization is required.
- ARM64 Windows PCs (Snapdragon and similar) are not supported: the launcher, runtime, and guest image are all x86_64, and setup stops with an explanation instead of blaming virtualization settings.
- GPU acceleration depends on the patched WINQ-EMU runtime and compatible Windows graphics drivers. Try Omarchy falls back to CPU rendering when that path is unavailable.
- USB management exists, but general physical-device compatibility remains unvalidated. V20 camera and microphone capture passed on the AMD test laptop; additional device combinations remain unverified, and arbitrary PCI passthrough is unsupported.
- Networking uses QEMU NAT. Services inside the guest are not exposed to the Windows network automatically.
- Host-folder sharing is not available with an external stock QEMU fallback.
- Text and image clipboard sharing work in both directions (images travel as
  PNG, up to 16 MiB). V20 streams file/folder clipboard transfers and accepts native
  Windows file drops without a blocking transfer window. Supported folders receive
  the files directly; other destinations fall back to Downloads. Direct drops into
  arbitrary guest applications remain unfinished.
- Portable mode is experimental pending external-drive and second-PC acceptance.
  Accelerated saved-session/RAM resume and bridged networking are not ready for use.
- The launcher boots its pinned kernel and initramfs from the release image, and the guest's pacman configuration holds the `linux` package so `pacman -Syu` and `omarchy-update` leave it alone. Kernel updates arrive with guest-image updates, which also carry the matching modules onto existing disks. Forcing a different kernel package into the guest leaves it out of sync with those boot files.
- Configuration export and restore are available through `try-omarchy-export`; see [the migration guide](MIGRATION.md). The published preview also supports [stopped-VM backup and restore](BACKUP.md) from Settings or command-line options. Reset can retain the old disk and offer a full backup first. Snapshots, restore-as-copy and rollback are also available.

Compatibility varies with Windows, CPU, GPU, and driver combinations. When reporting a problem, include those details and whether Try Omarchy selected GPU or CPU rendering.

See [v1 readiness](V1-READINESS.md) for the supported-scope target and outstanding
hardware acceptance. Current physical evidence centers on an AMD Windows 11 laptop.
