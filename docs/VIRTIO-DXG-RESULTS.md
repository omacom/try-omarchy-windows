# virtio-dxg: results before and after

> Runtime measurements reported by Claude through `8f1d75c`. Hardware,
> resolution, duration and workload matter when comparing these numbers.
> The [November 11 candidate](VIRTIO-DXG-INTEGRATION.md) awaits hardware testing.


Measured on one machine: an RTX 5090 Laptop GPU (driver 621.14) driving a
2560×1600 240 Hz panel, Windows 11, and the owner's Omarchy install, booted
read-only (QEMU snapshot) with 8 vCPUs and 8 GiB. "Native" means the same
test on Windows itself, on the same GPU (MSYS2 ffmpeg, a CUDA test built
against `nvcuda.dll`). The scripts are in
[virtio-dxg/bench](../virtio-dxg/bench/README.md). Design:
[VIRTIO-DXG.md](VIRTIO-DXG.md).

"After" is this branch. "Before" comes from earlier builds of the same
work, measured the same way:
- the desktop table: the first build that ran the desktop (black screen,
  virtio name), and for the 240 Hz rows the copy path that zero-copy
  scanout replaced;
- the graphics, compute and video tables: the full suite run of the build
  before this round of fixes (`final-vm.txt` in the bench output). Earlier
  still, NVENC ran at 72–95 fps with failures and NVDEC at 136 fps.

## The desktop

| | Before | After |
| --- | --- | --- |
| What renders the desktop | Black screen on the GPU path; CPU rendering only | Hyprland, its shell, Xwayland and apps on NVIDIA's D3D12 driver |
| GPU name in `lspci`, fastfetch, `eglinfo`, `vulkaninfo`, `vainfo`, `nvidia-smi` | "Red Hat, Inc. Device 107e" (virtio) | "NVIDIA GeForce RTX 5090 Laptop GPU" |
| Display mode | 1280×800 at 120 Hz, fixed | The host window's size and its monitor's refresh rate, followed live: 2496×1407 at 240 Hz in a window, the panel's 2560×1600 at 240 Hz fullscreen |
| How a frame reaches the window | Three full-frame CPU copies (GPU readback into the virtio-gpu buffer, into QEMU's surface, into SDL's texture) | Zero-copy: the window draws the compositor's own D3D12 texture, one GPU blit (0.04–0.06 ms) |
| Compositor at 2496×1407, 240 Hz | **61 FPS**, 16.4 ms per frame, 67 ms variance (Hyprland's overlay) | **238 page flips/s**; 99.1% of frames on time (1414 of 1427 in 6 s), 12 one frame late (kernel DRM trace, vkcube running) |
| Browser animation test (testufo, Chromium) | — | 220–240 fps, the owner's own test (about 200 before the host-thread and window changes) |
| Input polling (keyboard, pointer buttons, wheel) | every 10 ms when busy, 30 ms after a pause | every display frame: 4 ms at 240 Hz, 8 ms when quiet |
| Compositor freezes | Hyprland froze for good after seconds to hours (Mesa 26.2.4 slab-lock self-deadlock) | Fixed (upstream Mesa fix backported); 180 s of overlay, vkcube and glmark2 together without a missed check |
| How an application's frame reaches the compositor | `wl_shm`: a GPU readback in the application and an upload in the compositor | Zero-copy window buffers: the compositor samples the application's texture |
| Opening the Omarchy menu at 240 Hz (kernel DRM trace, 1.53 s) | 311 frames, 26 late, longest gap 50 ms | 346 frames, 5 late, longest gap 16.7 ms |
| Terminal window open / close animation | not played: the development VM booted with `tryomarchy.render=cpu`, for which Omarchy turns animations off | 111–114 frames over the 0.45 s animation, gaps of at most 8.3 ms |
| Chromium | software rendering (no WebGL); then on the GPU only through an Xwayland wrapper (unscaled window, no window buttons) | native Wayland, default flags: ANGLE on D3D12 (RTX 5090), OpenGL ES 3.1 |
| Time to a mapped window (test VM, 6 vCPUs) | — | foot 57–67 ms; Nautilus 1.2 s first, 0.21–0.25 s after; Chromium 1.4–2.0 s |

The pointer itself moves with the Windows cursor (QEMU turns Hyprland's
cursor into the host cursor), so it was never limited by these rates;
clicks, drags and keys were.

## Graphics

| Test | Before | After | Notes |
| --- | --- | --- | --- |
| glmark2 1080p off-screen (OpenGL 4.6, Mesa d3d12) | 1289 (the first desktop build: 42, then a hang) | 2255–2522 | |
| vkmark 720p windowed (Vulkan on Dozen, test VM) | 627 (frames copied through `wl_shm`) | 4840–6781 | Zero-copy Vulkan windows: swapchain images are window buffers |
| Direct3D 11 under Wine (wined3d on OpenGL, 200 draws per frame, `bench/d3d11tri.c`) | Xwayland exited | 231 fps on a 240 Hz display | GLX on the D3D12 driver |
| Vulkan under Wine on NVIDIA's own Windows driver (dxgvk, `wine/vkpresent.c`, clear + present, test VM) | no driver (Wine's Vulkan was Dozen, 1.2) | Vulkan 1.4.357; 2686–3253 fps immediate, 234 fps FIFO on a 240 Hz display | dxgvk.dll: NVIDIA ICD + zero-copy swapchain |
| Direct3D 11 through DXVK 3.1.1 on dxgvk (Proton's route, `bench/d3d11tri.c`) | no Vulkan driver for it | 1452-1876 fps; DXVK reports NVIDIA GeForce RTX 5090, driver 621.14 | dxgvk |
| Direct3D 12 through vkd3d-proton 3.1 on dxgvk (`bench/d3d12clear.c`) | none | about 2000 fps; ray tracing tier 1.2, shader model 6.6 | dxgvk, host CPU waits and GPU VA windows |
| glmark2 720p windowed | 313 | 1579–1757 (453–519 on `wl_shm`) | Zero-copy window buffers |
| vkmark 720p (Vulkan 1.2, Dozen) | 491 | 1026–1211 | |
| vkcube, Wayland | timed out | 300 frames in 0.8 s without vsync | |
| vkcube with vsync at 240 Hz | — | 202 fps | Limited by vkcube's own present (a CPU readback per frame), not the compositor |

DirectX 12, as the guest's applications see it (`d3d12test`): feature level
12_1, shader model 6.7, mesh shaders tier 1, variable-rate shading tier 2,
sampler feedback 0.9, wave ops (32 lanes), tiled resources and resource
binding tier 3. WSL2 on the same machine reports exactly the same. NVIDIA's
user-mode D3D12 driver for Linux offers no DXR (ray tracing) and no
feature level 12_2 there either.

## Compute

| Test | Native | Before | After | After vs native |
| --- | --- | --- | --- | --- |
| CUDA vector add, 16M floats (memory bandwidth) | 787–793 GB/s | 728 GB/s | 770–774 GB/s | 97.5–98% |
| CUDA launch + synchronize, median | 7.2 µs (WSL2: 13.4 µs) | 44 µs | 16.9–17 µs | |
| CUDA 1000 launches | 8.3–9.1 µs each | — | 17–18 µs each | |
| CUDA 1080p→4K scaling (ffmpeg `scale_cuda`) | — | 123 fps | 136–141 fps | |

## Video

Steady state (1200 frames minus 60, so start-up is excluded):

| | Native | Before | After | After vs native |
| --- | --- | --- | --- | --- |
| NVENC H.264 1080p | 591 fps | 333 fps | 596 fps | 101% |
| NVENC HEVC 1080p | 516 fps | 533 fps | 525 fps | 102% |
| NVDEC H.264 1080p | 2528 fps | 665 fps | 1827–2147 fps | 72–85% |

Whole runs (1200 frames, start-up included):

| | Native | Before | After |
| --- | --- | --- | --- |
| NVDEC decode | 793 ms | 1498 ms | 1277 ms |
| NVENC H.264 | 2414 ms | 1604 ms, failing | 3131 ms |
| NVENC HEVC | 2688 ms | 3686 ms | 3406 ms |
| NVENC AV1 | 2455 ms | 3886 ms, failing | 3210 ms |
| Transcode NVDEC→NVENC | 2358 ms | 7014 ms | 2893 ms |
| Start-up, NVDEC / NVENC | 309 / 449 ms | 1428 / 2454 ms | 710 / 1178 ms |
| Failures in 30 mixed sessions | 0 | 2 ("out of memory" locking bitstream buffers) | 0 |

Other media:

| | Before | After |
| --- | --- | --- |
| VA-API (Mesa d3d12) | 15 profiles; decoding crashed ffmpeg (the first desktop build: no VA-API at all) | 15 profiles; H.264 1080p decode 241 fps with start-up |
| VA-API encode | fails | **still fails**: D3D12's video encoder removes the device on the first frame. NVENC (ffmpeg's `*_nvenc`, OBS) works |
| JPEG 1080p decode, NVDEC (`mjpeg_cuvid`) | not measured (0.0 parse error) | 467 fps |
| NVIDIA libraries: OptiX, NGX, Optical Flow, NVML, NVCUVID, NVENC | load | load |

## Stability fixes behind these numbers

| Fault | Effect | Fix |
| --- | --- | --- |
| Mesa 26.2.4 freed suballocated buffers under its own slab lock | Hyprland froze for good during a texture upload | Upstream fixes `1761974a`, `8835df35` backported (`guest/mesa/0003`, `0004`) |
| Lock2 views mapped with the size of the first view | Corrupt mappings; NVENC "invalid param" | Each view mapped with its own size |
| Process exit freed GPU VA ranges the driver frees with their allocations | 11k STATUS_INVALID_PARAMETER per run, slowdowns | Ranges remember the allocation that created them |
| Views went through QEMU's memory API (a flatview rebuild under the BQL each) | 3.3 ms per view, vCPUs stalled | Mapped straight into the WHPX partition: 0.1–0.3 ms |
| OpenResource was not offered | The guest's D3D12 runtime removed the device when one Mesa screen opened another's texture | dxghost opens the resource on the guest device through an NT handle |
| Expected adapter-info probe failures logged as errors | About 25 serial-console lines per process start, each byte a VM exit | Debug level; other ioctl failures rate limited |
| No native fence descriptors (`EGL_ANDROID_native_fence_sync`) | Chromium ends a frame by asking EGL for a native fence, which is also what submits the frame; without the extension it passed its window buffer to the compositor with the drawing unsubmitted: an empty (black or transparent) native Wayland window | The D3D12 driver offers the extension to applications: a fence asked for its descriptor finishes the work and returns a signalled sync file |
| A window buffer's texture pairing ended with the allocation that made it | Browsers allocate with GBM, import the dma-buf, free the allocation and only then pass the dma-buf on: the compositor found no texture | The pairing lives as long as the dma-buf (the module holds its inode, not the buffer) |
| Window buffers gave GBM no KMS handle | Clients that export through `gbm_bo_get_handle` could not allocate | The buffer keeps its GEM handle |
| virtio-gpu's 256 MiB host memory limit | Every window buffer is also a virtio-gpu buffer: after a few large windows creation failed on the host (656 errors in one session) | The limit is 8 GiB in dxg mode (reserved, never touched) |
| GLX asked for a DRI3 hardware screen under the session's driver name (`kms_swrast`) | X11 OpenGL (Wine, Proton, older toolkits) fell back to indirect rendering: glxgears 125 fps | A DRI3 connection creates the kms_swrast screen: X11 clients render on the GPU into window buffers |
| An imported window buffer exported the texture's D3D12 handle instead of its dma-buf | Xwayland presents a client's buffer by passing it on; the compositor refused it and Xwayland exited (every X11 application gone) | The import keeps its dma-buf and exports that |
| Shared allocations kept the private data from before the host created them | NVIDIA's Windows Vulkan driver found no layout in a texture made in the VM and refused it (VK_ERROR_OUT_OF_DEVICE_MEMORY) | The guest keeps the data the kernel-mode driver completed |
| The host accepted CPU waits on at most 32 objects (the GPU-wait limit) | NVIDIA's driver under vkd3d-proton waits on up to 53; D3D12 device creation failed | CPU waits up to 4096 objects |
| All guest processes share the host process's GPU address space | A fixed-base reservation NVIDIA's driver prefers (12-28 GiB at 8-32 GiB) was taken by another guest process | A taken fixed base is granted at the same offset in a 1 TiB window of the guest process's own |
| The Windows Vulkan loader read the adapter's VulkanDriverName through the routed D3DKMT | NVIDIA's driver loaded a second time without dxgvk's presentation; D3D12 picked it | The bridge reports no Vulkan registry values; dxgvk is the adapter's driver |
| The debug overlay and idle blanking in measurements | Throttled clients (display off, locked session); Hyprland's overlay can deadlock in hyprgraphics | Benchmarks keep the display on and do not use the overlay |

## Where the rest of native performance goes

- **Vulkan windows.** Dozen's window system integration still presents
  through `wl_shm` (a readback per frame): vkcube with vsync reaches about
  200 fps of 240. OpenGL, GTK, Qt and Chromium windows are zero-copy.
