# App compatibility

Try Omarchy runs the full x86_64 Arch Linux environment used by Omarchy. It is not a reduced web demo or a compatibility layer.

## What works

- Omarchy desktop apps, themes, menus, keybindings, and screensavers
- Arch packages installed with `pacman`
- AUR packages installed with the preinstalled `yay` helper and the included `base-devel` toolchain
- Graphical Linux applications, including Visual Studio Code
- Web browsing and outbound networking through the Windows connection
- Audio, two-way clipboard sharing, and persistent files inside the guest
- Host-folder sharing with `-share <folder>` when the WINQ-EMU runtime is active

## Current limits

- Windows 11 with hardware virtualization is required.
- ARM64 Windows PCs (Snapdragon and similar) are not supported: the launcher, runtime, and guest image are all x86_64, and setup stops with an explanation instead of blaming virtualization settings.
- GPU acceleration depends on the patched WINQ-EMU runtime and compatible Windows graphics drivers. Try Omarchy falls back to CPU rendering when that path is unavailable.
- USB devices, host webcams, and arbitrary PCI devices are not passed through.
- Networking uses QEMU NAT. Services inside the guest are not exposed to the Windows network automatically.
- Host-folder sharing is not available with the stock QEMU CPU-only fallback.
- The launcher boots its pinned kernel and initramfs from the release image. Ordinary package installation is supported, but replacing the guest kernel independently can leave it out of sync with those boot files.
- There is not yet a built-in snapshot, export, or migration workflow. Keep important work somewhere you also back up outside the guest.

Compatibility varies with Windows, CPU, GPU, and driver combinations. When reporting a problem, include those details and whether Try Omarchy selected GPU or CPU rendering.
