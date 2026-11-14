# Mesa for the virtio-dxg guest

The guest's OpenGL, GBM, VA-API and Vulkan run on NVIDIA's D3D12 driver
through Mesa 26.2.6: the d3d12 Gallium driver (OpenGL 4.6, GLES 3.1, video)
and Dozen (Vulkan 1.3). Changes from upstream Mesa:

- **Scanout mirrors:** the compositor renders into KMS buffers whose D3D12
  textures the host window draws directly (zero-copy scanout); flushes that
  wrote one finish the GPU work, tracked per context.
- **Window buffers** (below): applications hand the compositor GPU textures.
- **Native fences:** `EGL_ANDROID_native_fence_sync`; a fence asked for its
  descriptor finishes the work and returns a signalled sync file (Chromium
  submits frames this way).
- **Resource safety:** scanout owners retained through borrowed wrappers,
  zero-ref weak lookups refused, failed uploads and constant-buffer updates
  reported instead of used, a failed GPU completion never publishes a CPU
  readback.
- **GLX on DRI3** for X11 applications (Wine, Proton, older toolkits), and
  imported window buffers that export their dma-buf again (Xwayland).
- **Dozen:** Vulkan 1.3 and what Blender needs beyond it (below), zero-copy
  Vulkan windows.

The versioned recipe in `versions/26.2.6/` pins the official source archive,
the DirectX-Headers commit, the complete diff and the before/after hash of
every changed file. The older numbered patches are source history; the build
uses only the recipe.

## Window buffers

Application windows are zero-copy too. On the render node no dumb buffer can
be made, so applications used Mesa's software winsys: a GPU readback in the
application and an upload in the compositor for every frame (`wl_shm`). Now a
shared image on the render node is a **window buffer**:

- a 2D virtio-gpu buffer exported as a dma-buf. Compositors validate client
  buffers on their DRM device (Hyprland calls `drmPrimeFDToHandle`), which a
  bare D3D12 shared handle fails; a virtio-gpu dma-buf passes, and its memory
  is never touched;
- a shared D3D12 texture the application renders in, paired with that dma-buf
  in the guest's dxg module (`LX_DXVIRTIOWINBUF`, register);
- on import, in any process, Mesa asks the module for the texture of the
  dma-buf it holds (lookup) and opens it: the compositor samples what the
  application rendered. Possession of the dma-buf is what grants the lookup.

Flushes that wrote a window buffer finish the GPU work, as scanout mirrors
do, so a committed frame is complete. Without the registry (an older module)
no window buffer is made, and the boot hook keeps sessions on `wl_shm`.
`guest/test/clientbuftest.c` runs both ends in one process.

Measured at 1280x720 on the RTX 5090: glmark2 windowed 1579-1757 FPS (254
with `wl_shm`); opening the Omarchy menu, 346 of 367 possible frames with 5
late (311 with 26 late). Chromium runs on the GPU as a native Wayland client
with its default flags (ANGLE on D3D12), where it needed Xwayland before.

## Vulkan 1.3 (Dozen)

Dozen, Mesa's Vulkan driver on D3D12, reported Vulkan 1.2. It now reports
1.3 with every feature 1.3 requires. Each piece translates directly to D3D12;
nothing is emulated in software:

- **Extended dynamic state 1 and 2.** The `vkCmdSet*` calls are the common
  runtime's; Dozen applies graphics state at draw time anyway, so it reads
  what the bound pipeline declares dynamic there. What D3D12 bakes into a
  pipeline state (cull mode, front face, depth and stencil state, depth
  bias enable, primitive restart, rasterizer discard) becomes part of the
  pipeline-variant key, the mechanism Dozen already used for stencil masks:
  the pipeline is built once from neutral values and each new combination is
  a D3D12 pipeline state made on first use and cached. Topology, viewport and
  scissor counts and vertex strides are command-list state.
  `vkCmdBindVertexBuffers2` is new.
- **Inline uniform blocks** are uniform buffers to the shaders. Their bytes
  live in a host-visible buffer per descriptor pool; a set gets a 256-byte
  aligned slice and a uniform-buffer descriptor pointing at it, in both of
  Dozen's descriptor modes. Writes, copies and update templates copy bytes.
- **Zero-initialized workgroup memory** takes the explicit shared-memory
  layout and clears it word by word before the shader starts.
- **maintenance4** (requirements without an object), `VkFormatProperties3`,
  shader demote and terminate, the Vulkan memory model, image robustness,
  pipeline creation cache control, the 1.3 limits.

`bench/vulkan13` checks these with rendered pixels and read-back buffers.

What Blender 5.2's Vulkan backend needs beyond 1.3, each a D3D12 feature:

- **Multiple viewports** with the viewport index written by the vertex
  shader (`SV_ViewportArrayIndex`, which NVIDIA's D3D12 driver accepts from
  any stage feeding the rasterizer); the Y/Z flip Dozen applies to positions
  is done per viewport.
- **Dual-source blending** (one attachment) and **logic ops**
  (`OutputMergerLogicOp`).
- **The last vertex as provoking vertex** (`VK_EXT_provoking_vertex`, per
  pipeline): flat inputs, integers included, are read at the last vertex with
  `GetAttributeAtVertex`. D3D12 orders the vertices of odd strip triangles
  differently, so those still take the wrong vertex.
- **Window formats and color spaces.** Window-buffer images are typeless,
  so sRGB views of 8-bit swapchains work; FP16 (`XB4H`) and 10-bit (`XB30`)
  window buffers; `VK_EXT_swapchain_colorspace` with sRGB and pass-through.
  The Wayland WSI leaves untagged sRGB surfaces alone, so an application
  that attaches its own color-management surface (Blender does) is not
  disconnected for a second one. In bindless mode sRGB views get no UAV.
- The external fence, memory and semaphore capability queries at instance
  level.

Dozen still lacks tessellation and a viewport index written by geometry
shaders; Vulkan 1.4 is the next step.

## Building

Build from a full repository checkout on Linux with the guest's build dependencies
available (X11/XCB, Wayland/protocols, VA-API, GLVND, DRM, compilers and Python venv).
The build does not install into the system or load a graphics library. It uses a
fresh private directory, hash-pinned Python tools, pinned DirectX-Headers and two
compiler workers by default (`JOBS=1..8`).

```sh
bash virtio-dxg/guest/mesa/build-in-guest.sh mesa-26.2.6.tar.xz mesa-virtio-dxg.tar
```

GLX/EGL/GBM/GLVND, d3d12/softpipe, VA-API and Dozen are built together. The tar
contains `lib/`, `share/`, licenses, the actual version and a data-only receipt
with source/recipe/SDK and staged-file hashes. The build directory is retained
for diagnosis; a failed build never replaces the requested output archive.

Rebuild the **initramfs and Mesa archive as a matched pair** with this branch's
`guest/test/mkdevinitramfs.py`. Its boot hook rejects an archive whose recipe,
contents or aliases differ, before selecting session graphics libraries. Files
are staged under `/run`, without replacing guest packages.

The guest reads host NVIDIA/WSL libraries from read-only shares at each boot;
there is no separate Linux NVIDIA package to upgrade for this dxg path.

## Testing

`guest/test` checks the payload staging (`python3 -m unittest`).
`bench/vulkan13` checks Dozen with rendered pixels; the results on hardware
are in [VIRTIO-DXG-RESULTS.md](../../../docs/VIRTIO-DXG-RESULTS.md).
