# OpenGL application acceptance — 2026-09-20

These checks ran inside the full Omarchy **QEMU guest on Windows**, first on a
disposable copy and then on the existing installed guest. They are separate from
the [WSL feasibility experiment](../WSL-GPU-EXPERIMENT.md). WSL was used as a
compiler for the custom Blender binary, not as its acceptance environment.

## Machine and build

- Windows 11 Pro, build 26200; Intel Core i9-14900KF; 64 GB host RAM.
- NVIDIA GeForce RTX 5080, Windows driver 616.92. Windows continued using it.
- Existing pinned WINQ-EMU runtime; 8 vCPUs and 12288 MiB guest RAM for these checks.
- Guest Mesa 26.2.3; native VirGL OpenGL 4.2 and GLSL 4.20, accelerated renderer
  `virgl (NVIDIA GeForce RTX 5080/PCIe/SSE2)`.
- Godot 4.7.2 Arch package; official SuperTuxKart 1.5 Linux x86_64 archive.
- Blender 5.2.2 LTS source `d13f752e3b9c4f8c261cda552b1021f8bcc0382c`,
  [readback patch](../../scripts/gpu/blender/buffer-storage-fallback.patch),
  Arch FFmpeg patch from packaging commit `45cd72aaf75ac3c013a71217b3e5719a517b87e5`.
  The [build recipe](../../scripts/gpu/blender/build-arch.sh) was run locally.
- Final installed Blender executable SHA-256:
  `dcb6669ab9ac6dfb71307ae161fbcb604c5fd15b310c3e69fea3512675bd56ae`.
  Locally produced archive SHA-256:
  `9025565ec9ca1b41c284e76aee77f6ab66abeec28dd811d84a2e6b2999b2f3fc`.

The final build retains FFmpeg, USD, MaterialX, OSL, Embree and path guiding.
It uses Arch system libraries and Python 3.14. CUDA/OptiX/HIP/oneAPI and Vulkan
are disabled. The regular distribution Blender remains installed alongside it.

## Results

| Check | Result |
| --- | --- |
| Blender interactive solid viewport | Pass: full editor displayed the editable cube in the installed guest. |
| Workbench render | Pass: 320×240 PNG, pixel/framing checks and visual inspection. |
| Eevee render | Pass with the patched build **and** conservative GPU profile; 320×240 PNG checked numerically and visually. |
| Cycles CPU render | Pass: 8-sample factory scene, 320×240 PNG. This is not GPU compute rendering. |
| Blender save/load | Pass: edited rotation and a custom property survived saving and loading the object from a `.blend` file. |
| Godot Compatibility 3D | Pass: rotating lit cube, 300 frames, rendered-image readback, accelerated VirGL adapter, exit 0. Repeated in the installed guest. |
| Godot editor | Pass: editor opened a Compatibility project and exited cleanly. Desktop project manager launched; new-project dialog selected Compatibility. |
| SuperTuxKart OpenGL | Pass: four-kart Lighthouse race, 1280×720 window, 30-second profiling mode, exit 0. |

The installed-guest SuperTuxKart log recorded 3602 frames in 31.443001 seconds,
114.556496 average FPS. The copied guest recorded 112.453552 FPS. These are short
smoke runs with the game's then-current settings, not controlled benchmarks or
a promise about other titles, resolutions, input devices or long sessions.
OpenAL logged unavailable RTKit/realtime scheduling; the race still completed.

## Why the Blender profile is experimental

Stock Blender failed to create its required OpenGL 4.3 context. The native
driver exposes the relevant 4.3 extensions but only 13 vertex uniform blocks;
4.3 requires 14. The opt-in profile checks that specific capability pattern and
sets `MESA_GL_VERSION_OVERRIDE=4.3` and `MESA_GLSL_VERSION_OVERRIDE=430` only for
the Blender process. This does **not** make the driver OpenGL 4.3 conformant.

A version override alone was insufficient: Eevee aborted in `glBufferStorage`
because the renderer does not expose persistent buffer storage. The source patch
uses Blender's existing synchronous SSBO readback path for that missing feature.
That removed the crash, but a later image inspection found corrupt Eevee output.
Blender's `--debug-gpu-force-workarounds` option produced the correct image; this
is therefore part of the tested profile. It disables HDR and some optional GPU
optimizations. The automated scene check was strengthened to reject corrupt
backgrounds and missing centered geometry, not merely nonempty output files.

All final scene checks were repeated with the installed binary and profile.
They establish a useful small-scene path, not correctness of every Blender
feature, material, add-on, compute workload or large production project.

## Remaining limits and portability

- Physical PCI GPU passthrough and CUDA/OptiX remain unavailable in this VM.
- Vulkan initialization failed on this driver/runtime combination. Godot
  Forward+/Mobile and Vulkan-dependent games are not claimed to work here.
- No Wine/Proton, anti-cheat, AAA game, or other physical GPU acceptance is claimed.
- No Windows GPU driver, global Mesa setting, hypervisor security setting or
  original guest disk was replaced to obtain these results.
- Capability gating and path quoting are portable and tested with simulated
  Intel/AMD/NVIDIA renderer strings. Actual other-hardware acceptance is still
  required; those unit tests are not hardware evidence.

See [installation and rollback](../GPU-APPLICATIONS.md). User data and existing
application entries are preserved; the profiles have distinct “(Omarchy)” names.
