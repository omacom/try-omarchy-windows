# Experimental Mesa D3D12/Wayland components

This archive contains the Linux Mesa component files built by the pinned
source workflow. `root/usr` preserves their installation layout and library
symlinks. `BUILD-RECEIPT.json` records the source commits, hashes, generation,
staging and interface checks. `licenses` contains the included upstream notices.

The archive is a development component. It contains no installer or activation
script, and supplies no matching guest kernel, broker, shim, Windows-native
runtime or proprietary NVIDIA files. It is not a complete Omarchy graphics
backend or a daily-use release.

Wayland and other system dependencies are not bundled. The compiler and system
dependency versions in the Ubuntu build can differ from the intended guest;
the receipt is build evidence, not proof of guest runtime compatibility.

The build does not load these Mesa libraries or run a VM, compositor or shader.
Normal-session behavior, driver stability, cleanup/recovery and performance
still require separate validation. The host-display testing hold remains in
force for the development machine where the incident occurred.

The original source is Mesa 26.0.8 from
[the Mesa archive](https://archive.mesa3d.org/mesa-26.0.8.tar.xz), with the pinned
graphics and upload-guard patches in
[the fork's source recipe](https://github.com/7Wdev/try-omarchy-windows/tree/windows-nvgpu-lab/experimental/windows-native-gpu/mesa).

No Windows driver or VM disk is included or modified by creating this archive.
