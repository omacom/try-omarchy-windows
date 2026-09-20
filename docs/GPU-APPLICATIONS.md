# Blender, Godot, and an OpenGL game

These optional application profiles target the existing QEMU/WINQ-EMU Omarchy
desktop. They use translated graphics on the Windows GPU. They do not provide
PCI passthrough, CUDA/OptiX, a WSL desktop backend, or universal game compatibility.

## What to launch

After installing the profiles below, search Omarchy's application menu for:

- **Blender (Omarchy)**: the separately built Blender with the tested OpenGL
  profile. Workbench and Eevee use graphics acceleration; Cycles uses the CPU.
- **Godot (Omarchy)**: OpenGL Compatibility for the editor and project manager.
  Newly created projects default to Compatibility. Existing projects retain their
  files; Vulkan-specific features still require a working Vulkan implementation.
- **SuperTuxKart (Omarchy)**: the installed game using its OpenGL renderer.

The [acceptance record](evidence/GPU-APPLICATIONS-2026-09-20.md) describes the
tests and their limits. Only one physical Intel/NVIDIA PC has been tested for
these profiles. Capability checks contain no GPU model or vendor whitelist;
that makes the code portable, not every untested driver compatible.

## Install the optional launchers

Run as the regular Linux desktop user, inside Omarchy. Start with current Arch
packages and matching libraries. For example:

```sh
sudo pacman -Syu --needed blender godot mesa-utils python
```

Keep Godot closed while installing its profile, so the running editor does not
overwrite the preference change. The helper supports Godot 4.x using the normal
Linux configuration directory, not Godot's self-contained portable mode.

```sh
python3 scripts/gpu/app_compat.py install --godot godot
python3 scripts/gpu/app_compat.py install --supertuxkart /path/to/run_game.sh
```

The game acceptance test used the official Linux x86_64 archive from
[SuperTuxKart 1.5](https://github.com/supertuxkart/stk-code/releases/tag/1.5),
with SHA-256
`57090b6c2163eb691f20104ae9712204acbb4e8341059dee0a5ff5315efc401b`.
Extract it into a user-owned directory and point the helper to `run_game.sh`.
An installed `supertuxkart` executable also works; that package variant has not
been used for this acceptance record.

## Build the Blender compatibility version

The tested VirGL driver exposes OpenGL 4.2, with the 4.3 extension set but only
13 vertex uniform blocks. Blender requires 4.3. A version override does not add
missing hardware features or make this driver conformant.

The explicitly enabled experimental profile checks the native renderer,
extension set and uniform-block limit before applying per-process 4.3/GLSL 430
overrides. It rejects software rendering and other insufficient configurations.
It also selects Blender's conservative GPU paths: without those paths, Eevee
can finish successfully but produce a corrupted image on the tested runtime.
HDR and some optional GPU optimizations are disabled by that Blender switch.
Native OpenGL 4.3+ drivers receive no version override or conservative switch.

The driver also lacks `GL_ARB_buffer_storage`. Stock Blender 5.2.2 calls that
API during Eevee readback and aborts. The seven-line
[source patch](../scripts/gpu/blender/buffer-storage-fallback.patch) selects the
existing synchronous SSBO readback path when persistent buffer storage is absent.
It does not advertise an unsupported buffer-storage extension.

Build on x86_64 Arch with the same library versions as the target guest. The
recipe pins Blender and Arch packaging commits, checks the Arch FFmpeg patch,
and retains a package/version list and complete source diff with the install.
It does not replace `/usr/bin/blender`. Allow several GB of build space and a
substantial compilation time; reduce `JOBS` if Windows or the guest needs memory.

```sh
sudo pacman -S --needed base-devel cmake ninja boost eigen git git-lfs mold \
  llvm libdecor wayland-protocols vulkan-headers
JOBS=4 bash scripts/gpu/blender/build-arch.sh "$HOME/blender-omarchy-build"
python3 scripts/gpu/app_compat.py install \
  --blender "$HOME/.local/opt/blender-5.2.2-omarchy/blender" \
  --blender-virgl-compatibility
```

This build keeps FFmpeg, USD, MaterialX, OSL, Embree and path guiding. CUDA,
OptiX, HIP, oneAPI and Vulkan are disabled for this QEMU profile. It relies on
Arch's shared libraries and Python, so an incompatible package update may require
rebuilding it. This is an optional experiment, not a bundled upstream release.

Check it in the desktop session using a disposable scene:

```sh
python3 "$HOME/.local/share/try-omarchy-apps/app_compat.py" run blender \
  --factory-startup --python scripts/gpu/blender/smoke.py -- \
  --output "$HOME/omarchy-blender-check"
```

The check renders all three engines, rejects blank/corrupt framing, and reloads
an edited object from the saved `.blend`. Inspect the PNGs as well: a small
factory scene cannot establish correctness of complex projects or every shader.
Look for `BLENDER_APPLICATION_CHECK_PASSED` and `result.json` only after all
checks complete. A source build finishing or a process exiting successfully is
not sufficient graphics evidence.

## Files and rollback

The helper writes only the current user's `try-omarchy-apps` directory and
`try-omarchy-*.desktop` entries under `XDG_DATA_HOME` (normally
`~/.local/share`). Godot's previous editor preferences are saved once as
`editor_settings-4.x.tres.before-omarchy` under its configuration directory.
It does not change graphics drivers, system launchers or global Mesa variables.

To stop using these profiles, remove their `.desktop` entries and launch the
original applications. With Godot closed, restore the saved renderer preference
if desired; restoring the whole backup also restores older unrelated preferences.
The separately built Blender and extracted game can be removed when no longer
needed. Keep your project files. Do not uninstall Omarchy to remove a profile.
