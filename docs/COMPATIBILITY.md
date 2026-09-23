# App compatibility

Try Omarchy runs the full x86_64 Arch Linux environment used by Omarchy. It is not a reduced web demo or a compatibility layer.

## What works

- Omarchy desktop apps, themes, menus, keybindings, and screensavers
- Arch packages installed with `pacman`
- AUR packages installed with the preinstalled `yay` helper and the included `base-devel` toolchain
- Graphical Linux applications, including Visual Studio Code
- Web browsing and outbound networking through the Windows connection
- Audio, two-way text, image, file and folder clipboard sharing, and persistent files inside the guest
- Windows battery and charging state inside Omarchy, and live return of unused guest RAM to Windows with the bundled runtime
- Approved Windows desktop apps in the Omarchy launcher, opening on the Windows desktop with a tray action to return
- Host-folder sharing through the recommended `Omarchy Shared` folder or
  `-share <folder>` when the WINQ-EMU runtime is available, including CPU rendering

## Current limits

- 64-bit Windows 10 or 11 with hardware virtualization is required.
- ARM64 Windows PCs (Snapdragon and similar) are not supported: the launcher, runtime, and guest image are all x86_64, and setup stops with an explanation instead of blaming virtualization settings.
- GPU acceleration depends on the patched WINQ-EMU runtime and compatible Windows graphics drivers. Try Omarchy falls back to CPU rendering when that path is unavailable.
- USB management exists, but general physical-device compatibility remains unvalidated. V20 camera and microphone capture passed on the AMD test laptop; additional device combinations remain unverified, and arbitrary PCI passthrough is unsupported.
- Networking uses QEMU NAT. Services inside the guest are not exposed to the Windows network automatically.
- Live guest RAM return uses the bundled r19 runtime. A separately installed `C:\WINQ-EMU` is user-managed and can be selected instead; older external builds may not support free-page reporting.
- Windows Hello approval for guest `sudo` is not implemented; guest password authentication remains available for personalized accounts.
- Audio playback and recording devices can be selected before boot. Switching the active guest stream to another Windows endpoint without restarting Omarchy is not implemented.
- Approved Windows apps open on the Windows desktop. Displaying a Windows app as an embedded Hyprland window is not implemented.
- Host-folder sharing is not available with an external stock QEMU fallback.
- Text and image clipboard sharing work in both directions (images travel as
  PNG, up to 16 MiB). V20 streams file/folder clipboard transfers and accepts native
  Windows file drops without a blocking transfer window. Supported folders receive
  the files directly; other destinations fall back to Downloads. Direct drops into
  arbitrary guest applications remain unfinished.
- Portable mode is experimental pending external-drive and second-PC acceptance.
  Accelerated saved-session/RAM resume and bridged networking are not ready for use.
- The launcher boots its pinned kernel and initramfs from the release image, and the guest's pacman configuration holds the `linux` package so `pacman -Syu` and `omarchy-update` leave it alone. Kernel updates arrive with guest-image updates, which also carry the matching modules onto existing disks. Forcing a different kernel package into the guest leaves it out of sync with those boot files.
- Configuration export and restore are available through `try-omarchy-export`; see [the migration guide](MIGRATION.md). [Stopped-VM backup and restore](BACKUP.md) are available from Settings or command-line options. Reset can retain the old disk and offer a full backup first. Snapshots, restore-as-copy and rollback are also available.

Compatibility varies with Windows, CPU, GPU, and driver combinations. When reporting a problem, include those details and whether Try Omarchy selected GPU or CPU rendering.

## Games, Blender, and Godot

The guest sees a virtio GPU. WINQ-EMU forwards supported OpenGL and Vulkan
commands to the Windows GPU through VirGL and Venus; this is shared graphics
acceleration, not physical PCI passthrough. The Windows graphics driver remains
in control of the GPU. The launcher cannot expose NVIDIA CUDA/OptiX by increasing
guest RAM or installing a Linux NVIDIA driver.

| Workload | What to check |
| --- | --- |
| OpenGL / Vulkan games | Test the actual title, required extensions, and performance. A working desktop does not establish game compatibility. |
| Windows games through Wine / Proton | Translation, graphics extensions, DRM, and anti-cheat impose additional title-specific requirements. |
| Blender | Test viewport and CPU rendering separately. CUDA/OptiX rendering is unavailable through this graphics path. |
| Godot | Test the selected OpenGL or Vulkan renderer with the actual project. |

Use `vulkaninfo --summary` and `glxinfo -B` inside the graphical guest session
to inspect the rendering devices. `llvmpipe` or `lavapipe` indicates software
rendering; a GPU boot record only establishes that the VM reached userspace
using the GPU launch path. It is not an application benchmark. The cached record
can also predate a driver update; its timestamp is shown in Settings.

Maximum performance allocates more currently unused host resources, but extra
vCPUs can increase WHPX scheduling overhead. Use Manual to compare allocations
with the same workload. Guest RAM, GPU mapping space, and dedicated VRAM are
different resources: the RAM control does not allocate a percentage of GPU VRAM.

Relevant platform references: [WINQ-EMU graphics support](https://github.com/cmspam/winq-emu),
[QEMU virtio GPU backends](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html),
and [Microsoft's Hyper-V GPU support limits](https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/troubleshoot-hyper-v-gpu-assignment-partitioning-passthrough-issues).

The [Intel / RTX 5080 acceptance record](evidence/RESOURCE-PROFILES-INTEL-NVIDIA-2026-09-20.md)
documents working OpenGL and CPU-rendering checks alongside Vulkan and Blender
viewport failures on that particular driver/runtime combination.

The later [GPU application acceptance record](evidence/GPU-APPLICATIONS-2026-09-20.md)
tests an optional Blender source patch and conservative application profile in
the same QEMU desktop: Workbench, Eevee, Cycles CPU and saved scene data pass the
small-scene checks. Godot Compatibility and a SuperTuxKart race also pass.
See [installation, build and rollback instructions](GPU-APPLICATIONS.md).
Vulkan, CUDA/OptiX and physical passthrough remain unavailable on that tested path.

The separate [WSL GPU feasibility report](WSL-GPU-EXPERIMENT.md) contains reusable
checks for other PCs and application results using Windows' WSL GPU bridge.
WSL is not a launcher backend: the tested full Hyprland desktop cannot start
against WSLg, even though individual GPU applications can work there.

Current physical evidence centers on an AMD Windows 11 laptop.
