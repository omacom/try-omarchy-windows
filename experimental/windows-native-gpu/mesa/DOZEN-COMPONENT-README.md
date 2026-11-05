# Experimental Dozen Vulkan components

This developer archive contains a Linux Mesa 26.0.8 Dozen ICD and loader
manifest, built from the public source recipe with patches 0005 through 0008.
`root/usr` preserves the staged layout. `BUILD-RECEIPT.json` records the exact
source, build options, dependencies, library metadata and file hashes.

The workflow compiles the complete driver and inspects its files. It never
loads the ICD, enumerates a GPU or runs Vulkan commands. Manifest and exported
loader-function checks establish packaging metadata, not API conformance or
compatibility with applications or the intended guest.

Stock Dozen's Linux Wayland WSI selects its software presentation path. This
recipe does not replace that path or implement GPU sharing with Hyprland.
Enabling the Wayland build does not establish GPU-backed window presentation.
The earlier compute and buffer-sharing diagnostics do not cover that boundary.

No installer, activation script, guest kernel, broker, shim, proprietary NVIDIA
runtime or Windows driver is supplied. System/Wayland dependencies are not
bundled and can differ from the target guest. Do not copy this archive over a
working runtime as an upgrade. It is not a complete backend or daily-use release.

Source and patches are available from [the public recipe](https://github.com/7Wdev/try-omarchy-windows/tree/windows-nvgpu-lab/experimental/windows-native-gpu/mesa).
Upstream license notices are included under `licenses`.
