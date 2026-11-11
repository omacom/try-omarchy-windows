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
