# Reproducible Dozen component build

Added November 5, 2026. This recipe builds and inspects files on a remote CPU
runner. A completed run for the current revision is required before treating
its build or packaging as verified. The local GPU/display testing hold remains
in force; this recipe does not perform runtime validation.

The completed [desktop component build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/41993914673)
uses `vulkan-drivers=[]`. Earlier [Vulkan compute](VULKAN-4K-COMPUTE.md) and
[typed buffer](VULKAN-GPU-BUFFER-ROUNDTRIP.md) evidence comes from separate private
builds. Those historical results do not verify this new public source recipe.

The workflow now defines a separate full Dozen build after the desktop archive
is verified. It applies the existing hash-guarded patches 0005 through 0008 in
order: FD export ownership, FD import lifetime, typed native GPU buffers and
allocation-admission errors. A fresh build directory compiles the complete
Vulkan driver and its shader compiler against the real pinned DirectX headers.
The graphics build and its archive retain their existing profile.

The Dozen profile uses Mesa 26.0.8, Wayland 1.26.0, protocols 1.47 and the same
pinned DirectX headers and Python build tools as the desktop build. It selects
only `microsoft-experimental` for Vulkan, enables the Wayland build platform and
disables Gallium, OpenGL, EGL, GBM, LLVM, video and graphics tests. The prefix is
`/usr`, libdir is `lib`, and installation stages into an owned CI destination.
Compilation uses two workers within the workflow's thirty-minute limit.

The [staging verifier](../experimental/windows-native-gpu/mesa/verify_mesa_dozen_stage.py)
reads source/build metadata and ELF files without loading them. It checks:

- The four patch files and all final Dozen source hashes from their manifests.
- The selected build options and pinned Wayland dependency versions.
- An x86-64 ELF shared library with the expected SONAME, no build RPATH/RUNPATH,
  no host paths in dependencies, and an exported `vk_icdGetInstanceProcAddr` function.
- A 64-bit loader manifest selecting `/usr/lib/libvulkan_dzn.so`, with the
  manifest API version derived from the pinned Vulkan header.
- Symlinks contained in the staging root and hashes of the staged files.

The [data controls](../experimental/windows-native-gpu/mesa/test_mesa_dozen_stage.py)
use copies of the newly compiled library. They define one valid case and ten
rejection cases: host/relative manifest paths, wrong API/architecture/format,
missing library, non-ELF data, an escaping symlink, wrong SONAME and a missing
loader entry point. These controls do not call Vulkan. The archive readback
compares all staged file hashes and symlink strings against the build receipt.
No control results are claimed until a completed run supplies evidence.

The proposed `mesa-dozen-wayland-components` artifact includes the staged Linux
files, receipt and upstream license notices. It includes no installer, activation
script, proprietary NVIDIA runtime, Windows driver, guest disk or matching
kernel/broker/shim. Wayland and other system dependencies are not bundled.

Stock Dozen's Linux Wayland WSI still selects its software presentation path.
This recipe does not change that flag or implement GPU buffer sharing and
synchronization with Hyprland. A compiled Wayland ICD does not establish the
required GPU-backed desktop. Vulkan graphics/presentation and application
conformance, CUDA, video, DLSS, hardware ray tracing, matched-native performance,
normal launcher sessions and daily-use stability remain unverified. Counts
remain **4/8, 0/4, 3/3**.
