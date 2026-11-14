# Experimental Mesa D3D12 fence FDs

The [guarded guest candidate builder](../../../docs/GUEST-MESA-CANDIDATE.md)
prepares an eleven-file Mesa update in a fresh private normal-root initramfs,
using archive/ELF data only. Completed controls for the revision and a private
assembly receipt are required. The recipe neither activates a runtime nor
establishes GPU/desktop stability.

The [full Dozen build recipe](../../../docs/MESA-DOZEN-SOURCE-BUILD.md) adds a
separate reproducible Vulkan component profile using existing patches 0005-0008.
A completed CI run for the current revision is required for build evidence.
It does not implement GPU-backed Wayland presentation or establish
application/driver stability.

Two independently opt-in patches are available: the private timeline-FD patch
below and an additive owned Linux sync-file/EGL callback patch. The latter is
verified in [two hardware runs](../../../docs/LIVE-GPU-EGL-NATIVE-SYNC.md),
with disabled-extension and timeline regressions. It does not implement general
sync-file interoperability, DRM/GBM or a compositor desktop.

The independent pinned `apply_mesa_upload_failure.py` patch corrects upload-buffer
failure ownership in Mesa 26.0.8. Its complete-uploader CPU regression reproduces
lost references and duplicate releases without a graphics runtime. The full
runtime and Quickshell crash still need validation; see
[failure cases and limits](../../../docs/MESA-UPLOAD-FAILURE-LIFETIME.md).

Apply `apply_mesa_constbuf_failure.py` after that ownership fix to guard the
state tracker's constant uploads against allocation/map failure. The recipe
requires both pinned source hashes. Its CPU regression checks all eight shader
stages and successful copies; runtime integration remains unverified. See
[guard, exact evidence and limits](../../../docs/MESA-CONSTBUF-FAILURE-GUARD.md).

Both complete affected C files also compile against real Mesa headers in the
Windows D3D12 configuration. That check produces object files only. A subsequent
[full Linux-target build](../../../docs/MESA-LINUX-CROSS-COMPILE-2026-11-04.md)
compiles D3D12/EGL/GBM with both guards and the captured private presentation
overlay, then compares 527 API exports and their versions. No library is loaded;
packaging, runtime and hardware validation remain open.

The prepared `mesa-source-build.yml` workflow compiles D3D12/EGL on a remote
Linux runner without loading it. Its Meson wheel is pinned by version and digest
in `source-build-tools.txt`.

Apply `apply_mesa_fence_fd.py` first, then `apply_mesa_native_sync.py` to that
exact pinned tree. The additive helper verifies both patch digests and every
changed/unchanged base file, dry-runs with zero fuzz and rejects mixed sources.
Use `D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE=true` before screen creation for the
new path; both native bridge hooks must also be present. That option is
independent of `D3D12_EXPERIMENTAL_FENCE_FD`. All defaults remain off.

## Owned opaque desktop presentation source

`apply_mesa_native_presentation.py` follows graphics patches 1-4. Patch 0011
publishes the same owned-presentation source used by the captured desktop build;
patch numbers 0005-0008 remain the separate Dozen work. Both native-buffer and
presentation options are disabled by default. The matching kernel/broker/shim
contract remains required; this does not implement KMS or general foreign imports.

Eight source recipe controls pass. The compile-only workflow now includes pinned
Wayland 1.26.0 and protocols 1.47, independent protocol generation and data-only
inspection of an owned Linux staging tree. The completed `6c652f3` run passes
that build, generation and staging contract, including the six loader aliases
and 527 API exports. No staged library is loaded or shipped as a usable runtime.
[Source, requirements and verification limits](../../../docs/MESA-OWNED-PRESENTATION-SOURCE.md).

## Native EGL image development patch

Apply `apply_mesa_native_images.py` after both existing timeline patches. It
checks the pinned Mesa 26.0.8 source and every predecessor/patched file hash,
rejects mixed or modified source before mutation, and verifies repeated application.
Enable `D3D12_EXPERIMENTAL_NATIVE_BUFFERS=true` before creating the screen, with
the matching kernel, broker, shim and native host. Defaults remain off.

This patch adds owned DMA-BUF EGL image import/export and an automatic shared
allocation hook. It advertises RGBA/RGBX/BGRA/BGRX with implicit opaque layout;
BGRX uses BGRA native storage. It provides no explicit linear modifier, foreign
DMA attachment, CPU mapping, GEM/KMS handle or implicit reservation synchronization.
The shared-allocation hook still requires actual GBM validation.

Use `WDDM_BRIDGE_TEST_NATIVE_IMAGES=1` with `build_opengl_probe.sh` for the
separate image diagnostic. Optional BGRA, RGBX, BGRX, small-size and disabled
variants are selected with `WDDM_BRIDGE_TEST_NATIVE_IMAGE_*` options in that
script. The native image path requires both kernel enablement and the exact
host interop capability response; the Mesa environment option alone is insufficient.

[Development hardware evidence and limits](../../../docs/LIVE-GPU-EGL-NATIVE-IMAGE.md)
include four format runs, the disabled control and retained failures. This does
not establish GBM, a DRM device, compositor sampling, a full 4K shader viewport,
Hyprland or sustained performance.

## Private timeline patch

This opt-in patch targets Mesa **26.0.8** and the fork's private QEMU/Windows
NVIDIA bridge. It adds the missing Linux Gallium fence-FD callback for a native
D3D12 timeline fence exported through the bridge's authenticated kernel FDs.
It also corrects timeline type queries and pre-import type changes, rejects
operations on an unimported semaphore, and preserves the old fence and caller's
FD when import fails. Successful import transfers FD ownership to GL.

This is a private native-fence FD contract. It does **not** implement binary
Vulkan semaphore interoperability, Linux `sync_file`, dma-buf, DRM/KMS or
compositor buffer synchronization. Binary and foreign FDs are rejected. It does
not establish GL extension conformance or general Vulkan interoperability.

## Apply and build

Download the official [Mesa archive](https://archive.mesa3d.org/mesa-26.0.8.tar.xz)
and check its SHA256 against `fence-fd-patch.json`. Extract to a separate source
directory, then run:

```sh
python3 apply_mesa_fence_fd.py /path/to/mesa-26.0.8
```

The script checks every source and patch digest, dry-runs the patch with zero
fuzz, applies it, and checks the resulting digests. Repeating it on the exact
patched tree only verifies it. Mixed, changed or wrong-version sources fail.
Build a non-GLVND, D3D12-only Mesa installation with EGL and without GLX, LLVM,
Vulkan or video drivers, as in the existing OpenGL diagnostic.

On Linux, semaphore support remains disabled unless
`D3D12_EXPERIMENTAL_FENCE_FD=true` is set before screen creation. Windows keeps
its existing fence capability path. The diagnostic enables this explicitly;
normal app startup does not enable it.

## Compile the genuine GL test

```sh
WDDM_BRIDGE_TEST_MESA_FENCE_FDS=1 \
WDDM_BRIDGE_MESA_SOURCE=/path/to/mesa-26.0.8 \
WDDM_BRIDGE_DIRECTX_HEADERS=/path/to/DirectX-Headers \
sh ../build_opengl_probe.sh /path/to/private-mesa-install /path/to/new-output
```

Package the probe with the existing OpenGL initramfs builder, a current bridge
shim, and the guest aperture module. Private NVIDIA runtime files stay local.
Run only in the owned, disk-free QEMU/WHPX harness with the native 4K profile.

The test explicitly requests OpenGL profile **version 2**. Its fixed contract
allows 384 aggregate objects, 16 hardware queues and 192 vendor allocations.
Version 1 and other sessions retain their earlier 256-object, 8-queue and
capability-selected 96/128-allocation limits. The 96 sync-object, 128 CPU-slot,
GPU/CPU byte, 16 pending-command, 64 total-command and 10,000-request limits
remain in effect. Version 2 is negotiated before any native objects exist;
malformed, late, repeated and unsupported negotiations fail.

The helper borrows Mesa's real D3D12 device through
`MesaGLInteropEGLQueryDeviceInfo`. It imports an actual native timeline through
`glImportSemaphoreFdEXT`, then exercises `glSignalSemaphoreEXT` and
`glWaitSemaphoreEXT` with `GL_NV_timeline_semaphore` parameters. An independent
D3D12 copy queue releases the future wait. A normal GL pixel audit remains
separate from the semaphore ordering evidence.

The FD ownership and timeline semantics follow the
[Khronos FD extension](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects_fd.txt)
and [timeline extension](https://registry.khronos.org/OpenGL/extensions/NV/NV_timeline_semaphore.txt).
Only the bounded diagnostic's observed behavior may be claimed as verified.

[Hardware evidence and known limitations](../../../docs/LIVE-GPU-MESA-FENCE-FD.md)
retain two genuine QEMU timeline runs, the disabled-feature regression, a
successful legacy regression and an unresolved legacy process-teardown failure.
