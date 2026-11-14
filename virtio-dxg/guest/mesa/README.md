# virtio-dxg Mesa 26.2.6 candidate

The November 11 recipe keeps one completed sync file per D3D12 screen and
duplicates it for native-fence exports after producer completion. Rebuild the
Mesa archive and initramfs together because the recipe digest has changed.
The previous CI archive at `3718b3a` predates the native Wayland changes.

This branch combines Claude's KMS mirrors and compositor zero-copy scanout with
our resource-ownership, upload, constant-buffer, fence and Dozen FD guards.
It uses the real guest dxg kernel ABI, not the older private userspace broker.
The first cohort at d717dfa compiled completely and its CPU controls passed,
but runtime-archive acceptance failed because its receipt also listed SDK
headers not shipped in the tar. The corrected receipt and the submitted-fence
guard have since **run on hardware** (RTX 5090 Laptop GPU, 2026-11-10): the
Omarchy desktop, zero-copy scanout, the CUDA, video and D3D12 checks and a
60 s compositor stress pass on this cohort. That run found one fault, fixed
since: the staged directory kept mkdtemp's 0700 mode, so the session could
not load it. See
[status and performance limits](../../../docs/VIRTIO-DXG-INTEGRATION.md).

The versioned recipe in `versions/26.2.6/` pins the official source archive,
DirectX-Headers commit, complete combined diff and before/after source hashes.
Source `VERSION` remains 26.2.6. The 26.2.4 deadlock backports in historical
`0003`/`0004` are already upstream and are not applied twice. The older numbered
patches are retained as source history; the build uses only the pinned recipe.

The new recipe also tracks mirror writes per context, retains the scanout owner
through borrowed wrappers and rejects zero-ref weak lookups. Up to 16 pending
mirrors avoid a per-flush heap list. A failed GPU completion cannot publish a
CPU readback.

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

## Building the inactive cohort

Build from a full repository checkout on Linux with the guest's build dependencies
available (X11/XCB, Wayland/protocols, VA-API, GLVND, DRM, compilers and Python venv).
The build does not install into the system or load a graphics library. It uses a
fresh private directory, hash-pinned Python tools, pinned DirectX-Headers and two
compiler workers by default (`JOBS=1..8`). The caller must approve a test/build
batch before running it in this project.

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
are staged under `/run`, without replacing guest packages. Old receipt-free
26.2.4 archives are deliberately not accepted by this new hook.

The guest reads host NVIDIA/WSL libraries from read-only shares at each boot;
there is no separate Linux NVIDIA package to upgrade for this dxg path. A Windows
driver update changes that hardware baseline and requires a new approved runtime
check. Mesa version compatibility cannot be proven by renaming its library.

## Verification boundary

The prepared remote workflow builds the complete cohort and runs inert boot-data,
actual-source mirror/fence and mock-host controls. No ICD or graphics ELF is
loaded. The first batch passed those controls/build but rejected its archive; the current follow-up needs a new approved batch. Hardware correctness,
accurate animations, CPU/app-launch gains, full Chromium acceleration, 4K and
sustained cleanup/recovery require separately approved tests.
