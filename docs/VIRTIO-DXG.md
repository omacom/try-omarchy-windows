# virtio-dxg: driver-level NVIDIA virtualization on a Windows host

The integration includes native Wayland and shared application buffers through
`8f1d75c`. See the [results](VIRTIO-DXG-RESULTS.md) and the
[November 11 integration record](VIRTIO-DXG-INTEGRATION.md) for tested limits.

## The short version

virtio-nvgpu's idea is to forward the **GPU driver's kernel boundary** instead
of a graphics API, so the guest runs NVIDIA's own user-mode driver and frames
never cross the VM boundary. That idea carries over to a Windows host. Its
code mostly does not, because the boundary it forwards does not exist on
Windows.

| virtio-nvgpu (Linux host) | virtio-dxg (Windows host) |
| --- | --- |
| Guest runs NVIDIA's Linux driver (`libvulkan_nvidia`, `libcuda`, NVENC) | Guest runs NVIDIA's WSL driver (`libcuda`, `libnvwgf2umx` D3D12, NVENC, NVDEC, NGX, OptiX) |
| Boundary: RM ioctls on `/dev/nvidia*` | Boundary: dxgkrnl calls on `/dev/dxg` |
| Guest module copies ioctls over virtio | Microsoft's `dxgkrnl` module, VMBus replaced by virtio |
| Host backend replays ioctls on the host's `/dev/nvidia*` | Host backend (`dxghost`) replays them with Windows `D3DKMT*` |
| Control queue, event queue, GPU memory window | Same three pieces |

### Why the Linux `/dev/nvidia*` backend cannot be ported

On Windows the NVIDIA kernel driver is a WDDM driver. It does not accept
Linux RM ioctls, and Windows does not let user mode own GPU channels and
memory the way NVIDIA's Linux user-mode driver expects; dxgkrnl owns
scheduling, residency and GPU virtual memory. The only route to RM from user
mode is vendor-private `D3DKMTEscape` traffic, which is undocumented, changes
with each driver and is not a translation of the Linux ABI. Building an RM
translator on it would require an undocumented, vendor-specific translation
and a separate compatibility effort. GPT's earlier investigation reached the same conclusion
([WINDOWS-NVIDIA-BACKEND.md](WINDOWS-NVIDIA-BACKEND.md)).

### The equivalent boundary Windows does have

NVIDIA ships a Linux build of its user-mode driver that talks to the Windows
kernel driver through dxgkrnl. That is how WSL2 gets CUDA, D3D12, NVENC and
NVDEC. On this machine (RTX 5090 Laptop, driver 32.0.16.2114) the driver store
folder `nvami.inf_amd64_*` contains `libcuda.so.1.1`, `libnvwgf2umx.so`,
`libnvidia-encode.so.1`, `libnvcuvid.so.1`, `libnvidia-ngx.so.1`,
`libnvdxdlkernels.so`, `libnvoptix_loader.so.1` with `nvoptix.bin`, the PTX JIT
and the shader compiler. Forwarding dxgkrnl is the supported, driver-level
counterpart of virtio-nvgpu.

### Going further: NVIDIA's Windows driver in the guest

The WSL build has CUDA, D3D12 and video, but no Vulkan and no OpenGL. The
*Windows* user-mode driver has everything: `nvoglv64.dll` is NVIDIA's full
Vulkan 1.4 and OpenGL 4.6 driver, with hardware ray tracing, DLSS
(`VK_NVX_binary_import`), Reflex (`VK_NV_low_latency2`) and optical flow. It
also talks to the kernel only through D3DKMT, the same dxgkrnl interface.
Under Wine in the Linux guest, with Wine's D3DKMT calls sent to `/dev/dxg`,
it records command buffers natively in the guest and submits them to the
host's NVIDIA kernel driver. This is how a Windows VM with Hyper-V GPU
partitioning works, minus the Windows guest. Proton games (DXVK, vkd3d-proton)
would need compatible Wine/Proton hooks and presentation; the current
compute prototype does not demonstrate game execution.

[`virtio-dxg/wine/`](../virtio-dxg/wine) is the working prototype:
- `kmt_bridge.c`: D3DKMT on `/dev/dxg`, one ioctl per call, plus the legacy
  v1 calls, events and device idle.
- `kmt_display.c`: the GDI display devices the driver uses to find its
  adapter.
- `nvvkprobe.c`: loads the driver from the user's own driver store and runs a
  verified compute test.

Measured on the RTX 5090 Laptop GPU (driver 621.14), with WSL2's `/dev/dxg`
as the test bench until the VM is up. The baseline is the same binary run
natively on Windows:

| | Native Windows | Linux guest, bridged |
| --- | --- | --- |
| Device | RTX 5090 Laptop GPU, Vulkan 1.4.357 | same |
| Ray tracing pipeline, ray query, mesh shaders | yes | yes |
| DLSS / Reflex / optical flow extensions | yes | yes |
| Device extensions | 295 | 294 |
| GPU time of a 4M-invocation compute dispatch | 0.66–0.76 ms | 0.65–0.75 ms |
| Back-to-back dispatches (how frames run) | 0.69–0.76 ms each | 0.68–0.77 ms each, 96–97% GPU-bound |
| Submit-to-signal round trip | 0.71–0.79 ms | 0.77–0.83 ms |
| CPU cost of one submission | 10.5 µs | 22–25 µs |
| Result check | 4194304 / 4194304 | 4194304 / 4194304 |

GPU-bound throughput is the hardware's. What is left is per-submission CPU
cost (WSL2's VMBus round trip plus Wine), about 12 µs, which a game pays a few
times per frame. virtio-dxg submits without waiting for a reply.

How the driver finds its GPU took reverse engineering, done for
interoperability only: no driver file is modified or redistributed, and the
user's installed driver is used.
1. It enumerates GDI display devices (`EnumDisplayDevicesA`) and keeps one
   whose DeviceID contains `VEN_10DE`. In GPU-P mode it also accepts the
   Hyper-V/RDP display devices.
2. It calls `CreateDCA` on that device, then `D3DKMTOpenAdapterFromHdc`.
3. It reads the adapter's private data (`'NVDA'` header, version 1, 0xCDF8
   bytes) to detect paravirtualization.

Wine reports the Linux display stack's GPUs, so the bridge presents Wine's
displays as belonging to the dxg GPU, with PCI IDs and name read from the
kernel. It also needs:
- the driver loaded from its DriverStore path;
- the DRS profile database in `System32\drivers\NVIDIA Corporation\Drs` and
  `ProgramData\NVIDIA Corporation\Drs`;
- the legacy v1 calls (`CreateAllocation`, sync objects);
- `WaitForIdle`, emulated with per-context fences.

## Design

```text
Guest (Arch, kernel 7.2)                     Host (Windows, QEMU/WHPX process)
──────────────────────────────────           ─────────────────────────────────
App / Hyprland
NVIDIA WSL user-mode driver (unmodified)
Patched Mesa d3d12 / Dozen / d3d12 video
  │ ioctl on /dev/dxg
  ▼
dxgkrnl (Microsoft, unmodified above
the transport)
  │ dxgvmbus messages
  ▼
dxgvirtio.c ── ctrl virtqueue ─────────────► virtio-dxg-pci ─► lane threads
            ◄─ event virtqueue ───────────── (per guest process)
                                                  │
                                                  ▼
                                               dxghost ─► D3DKMT ─► NVIDIA KMD
            ◄═ IO-space BAR ════════════════ Lock2 views and fence pages,
               (guest-physical mappings)      mapped with WHvMapGpaRange
```

What crosses per frame: one asynchronous submission (no reply) and the
fence signals. Command buffers, uploads and render targets live in GPU or
host memory the guest already has mapped. Fence values are read straight from
mapped pages. CPU waits are armed host-side and complete through the event
queue, so a waiting guest thread costs no host thread.

### Where it beats WSL2's path

- **Presentation.** This backend keeps the compositor scanout on the GPU: the guest's frame stays a GPU resource
  and is shown in the existing SDL window (milestone 4 below).
- **Submission.** Submits and signals are fire-and-forget; requests of
  different guest processes run in parallel lanes, and the vCPU that rings
  the doorbell never waits on GPU work.
- **Memory.** Lock2 maps an allocation once; large views are 2 MiB aligned so
  the guest can use huge pages. Write-combined host memory is mapped
  write-combined in the guest (correct for non-snooped GPU memory), cached
  memory stays cached (fast readback).

### What is kept from virtio-nvgpu

The control/event queue split, the window ("IO space") for GPU memory, the
"one message per many frames" principle, event-driven wakeups instead of
polling, handle translation scoped to the owning guest process, and a backend
that does not depend on any particular VMM (`dxghost` takes map, unmap and
event callbacks, like virtio-nvgpu's `device/` traits).

### What is kept from the previous fork work

GPT's lab established on this exact hardware that user-mode D3DKMT accepts the
NVIDIA WSL driver's private data for devices, contexts, allocations, hardware
queues and fences, that Lock2 views and fence pages can be mapped into a WHPX
guest, and which adapter adaptations the driver needs. `dxghost` relies on
those results. The difference is the guest side: instead of an `LD_PRELOAD`
interposer, a userspace broker and a TCP socket, the guest runs the real
kernel driver, so every process (sandboxed Chromium, Flatpak, static
binaries) gets `/dev/dxg` without patching, and the transport is a virtqueue
inside QEMU. Nothing under `experimental/` was removed.

## Feature matrix

What this path can and cannot reach. "Native" means NVIDIA's own driver
executes the work; GPU-bound performance is then the hardware's.

| Capability | Path | Current evidence / limit |
| --- | --- | --- |
| CUDA | NVIDIA WSL `libcuda` | Reported GPU bandwidth 97.5–98% native; launch/synchronize about 17 µs vs 7.2 µs native. Not a universal 93% result. |
| NVENC / NVDEC | NVIDIA WSL video libraries | NVENC steady-state samples near native; NVDEC reported at 72–85%. Codec/session limits still apply. |
| D3D12 | NVIDIA WSL `libnvwgf2umx.so` | Reported feature level 12_1 / shader model 6.7. **DXR is not offered by this path.** |
| OptiX | CUDA / `nvoptix.bin` | Library loading reported; representative RT workload acceptance open. |
| Windows Vulkan under Wine | NVIDIA Windows `nvoglv64.dll` with prototype D3DKMT hooks | Reported verified compute dispatch. Windowed games, swapchains and general Proton compatibility remain open. |
| Vulkan ray tracing | Same Windows-driver prototype | RT extensions reported; actual RT workloads not exercised in the documented results. |
| DLSS / Reflex / frame generation | Windows-driver prototype and NGX | Extensions and library loading are not proof of working game integration. Workload validation remains open. |
| Windows OpenGL | NVIDIA Windows driver under Wine | Not tested. |
| Linux OpenGL | Mesa d3d12 | Reported OpenGL 4.6 samples and compositor rendering. Accurate animations and sustained stability remain open. |
| Linux Vulkan | Mesa Dozen on D3D12 | Vulkan 1.3 with all required features (extended dynamic state, inline uniform blocks, maintenance4, ...), checked by `bench/vulkan13`; zero-copy windows. Not a conformance claim; no tessellation, multi-viewport, dual-source blend or logic op; 1.4 open. No native NVIDIA Linux ICD. |
| VA-API decode | Mesa d3d12 video | Reported H.264 sample; Chromium codec-specific hardware decode still needs verification. |
| VA-API encode | Mesa d3d12 video | **Blocked: first frame removes the device in the reported test.** Chromium's integration wrapper disables accelerated video encode. NVENC is a separate working sample path. |

NVIDIA's native Linux Vulkan driver expects the Linux RM kernel ABI, which
this dxg bridge does not implement. The Wine compute prototype is promising,
but it does not establish native Linux Vulkan, DLSS or ray-traced games.

## Status

| Piece | State |
| --- | --- |
| Wire contract `protocol/virtio_dxg.h` | Done |
| Guest: `dxgkrnl` over virtio (`guest/`) | **Running in the Omarchy VM** (kernels `7.2.9-arch1-1` and `7.2.15-arch1-1`, Omarchy 4.0.4 and 4.5.0 with Hyprland 0.56.2 and 0.57.2), loaded by the development boot hook. |
| Host: `dxghost` executor (`host/`) | **Running against NVIDIA's driver** inside QEMU: CUDA, D3D12, OpenGL, Vulkan (Wine) and the whole desktop. |
| QEMU device `virtio-dxg-pci` (`qemu/`) | **Built** (development build and the full winq-emu runtime via `qemu/build-runtime.sh`). |
| NVIDIA Windows Vulkan driver in Linux (`wine/`) | **Working in the Omarchy VM over virtio-dxg** (and on WSL2's `/dev/dxg`): device creation, guest-side shader compile, compute verified, 96–97% GPU-bound like native. Every D3DKMT call the driver makes succeeds. Prototype hooks; to become a Wine/Proton patch. |
| Whole path in the real guest | The user's Omarchy disk booted read-only (QEMU snapshot), `dxgkrnl_virtio` loaded by an initramfs hook (`guest/test/mkdevinitramfs.py`), NVIDIA's driver store shared read-only over 9p. |
| Launcher option, guest image staging | **Working**: `TryOmarchy.exe -render dxg` (below). |
| Whole desktop on the GPU (Hyprland, shell, Xwayland, apps) | **Working**: rendered on NVIDIA through Mesa d3d12 with KMS mirrors (`guest/mesa/`). |
| Desktop presentation (zero-copy scanout) | **Working**: the window draws the compositor's D3D12 textures (below); 238 of 240 frames/s at 2496×1407, 240 Hz. |
| Guest display mode | **Follows the host**: the window's size, its monitor's refresh rate (240 Hz on this panel). |
| Sharing between devices and processes (`OPENRESOURCE`) | **Working** for shared resources (NT-handle sharing on the host). |

Measured results, before and after: [VIRTIO-DXG-RESULTS.md](VIRTIO-DXG-RESULTS.md).

Not offered yet, with exact replies: existing-sysmem allocations
(`VK_EXT_external_memory_host`), standard allocations, statistics, optional
dxgkrnl features.

### What the first VM runs taught

The kernel driver's caller is the VMM, an ordinary Windows process, where
under WSL2 it is a Hyper-V vGPU. `dxghost` therefore gives the guest what a
vGPU guest gets:
- the GPU-PV flag in NVIDIA's adapter private data (offset 0xCCC8);
- refusal of the NVIDIA escape that maps a timestamp page into the caller
  (`NVGL` 0x1000077), so the driver uses `QueryClockCalibration` as under
  WSL2.

Protocol details WSL2's host tolerates and `dxghost` now does too:
- commands whose trailing array is empty;
- in-place replies at the guest's full command length, including struct
  tail padding;
- CPU-notification sync objects and `EnqueueCpuEvent` signals, forwarded
  as guest events;
- zero-state GPU VA maps (no allocation) inside the caller's reservations.

Fence wake-up, measured with `guest/test/dxgtest`:

| | WSL2 VMBus | virtio-dxg |
| --- | --- | --- |
| Blocked wait woken by a signal, median | 45.9 µs | 33.2 µs |
| Signal + wait round trip, best | 30.1 µs | 29.9 µs |

### The desktop on the GPU

Boot argument `virtio_dxg.desktop=1` runs the user's Omarchy session on the
host GPU. Hyprland, quickshell and Xwayland load NVIDIA's D3D12 driver
(`libnvwgf2umx.so`) through Mesa's d3d12 Gallium driver, built with
virtio-dxg's KMS-mirror patch ([guest/mesa](../virtio-dxg/guest/mesa/README.md)).
Clients take Mesa's software winsys, as under WSLg: they render on d3d12 and
hand the compositor `wl_shm` buffers.

Verified in the VM (snapshot boot of the user's disk):
- wallpaper, bar, terminal and Chromium on screen, composited on NVIDIA;
- `gbmtest`: GPU frame in the scanout buffer at `glFlush`, about 3 ms at
  1280×800;
- six rounds of opening and closing 18 windows with GPU tests in between:
  the compositor stayed up, QEMU stayed responsive, and no driver events were
  logged on the host.

That run also found a QEMU use-after-free. An IO-space view's memory region
was freed while flatviews pending RCU destruction still pointed at it; a
desktop maps and unmaps views fast enough to hit it within seconds. Views are
now QOM objects that own their region, so the last flatview frees them, the
same pattern virtio-gpu uses for blob mappings.

The original software fallback was subsequently addressed with Xwayland and
ANGLE OpenGL while retaining Chromium's sandbox. The integration candidate
also routes the installed menu entry through that wrapper. Full browser/video
hardware acceleration is still unverified, and applications still hand frames
to the compositor through shared memory. The obsolete `chromium_gpu=1`
sandbox workaround is not part of the current boot hook.

### Zero-copy scanout

The compositor's scanout buffers are virtio-gpu 2D dumb buffers, each
mirrored by a GPU texture. Copying every frame from the texture into the
buffer, then into QEMU's surface, then into SDL's texture held Hyprland to
61 FPS at 2496×1407 on a 240 Hz panel. Now:

1. Mesa (`guest/mesa/0005`) creates scanout mirrors as shared D3D12
   textures and links each to its buffer: `LX_DXVIRTIOSCANOUT` on
   `/dev/dxg` takes the texture's shared handle and the buffer's virtio-gpu
   resource id. When the host accepts, flushes stop copying and only finish
   the GPU work. The compositor imports its buffers on a second GBM device;
   that import wraps the same texture (OpenResource on the host).
2. The guest module sends the link on the device channel
   (`VIRTIO_DXG_CHANNEL_DEVICE`) with the resource's host handle.
3. QEMU opens the resource as an NT handle (`dxghost_share_resource`),
   imports it into the SDL window's OpenGL context
   (`GL_EXT_memory_object_win32`, BGRA8), and virtio-gpu, through hooks
   `qemu/prepare.py` adds (`include/hw/display/virtio-gpu-scanout.h`),
   skips the guest-memory copy and the surface of linked buffers: each frame
   is `dpy_gl_scanout_texture`/`dpy_gl_update`, a GPU blit of 0.04–0.06 ms.
   Cursor-sized buffers stay in memory, where virtio-gpu's cursor reads them.

It needs `-display sdl,gl=on`, which the launcher's dxg mode uses; on any
other display the links are refused and the copies stay. With
`DXGHOST_STATS=1`, QEMU logs the presented frame rate and the gaps longer
than 1.5 frames every 10 s.

### Zero-copy windows

Applications reach the compositor without copies as well: their window
buffers are virtio-gpu dma-bufs paired with shared GPU textures in the guest
module, which the compositor's Mesa opens on import
([guest/mesa](../virtio-dxg/guest/mesa/README.md#window-buffers)). The boot
hook selects this when the module offers it (`virtio_dxg.winbuf=0` keeps
`wl_shm`): GL applications then run through Mesa's KMS winsys on the render
node, and Chromium is a native Wayland client on the GPU with no wrapper.

### Starting it from the launcher

`TryOmarchy.exe -render dxg` boots the installed Omarchy on virtio-dxg. It is
opt-in. Normal launches keep the bundled runtime, the GPU (virgl) and CPU
modes, their render history and upstream updates. A dxg launch records no
render result, so it never changes what `auto` picks.

The window is SDL's OpenGL display, which zero-copy scanout draws into. The
guest's display mode follows it: the window's size in physical pixels
(fullscreen is the panel's native resolution) and the refresh rate of the
monitor it is on, updated when the window moves to another monitor.

The mode needs three files in `<data>\virtio-dxg\`:

| File | Made by |
| --- | --- |
| `runtime\bin\qemu-system-x86_64w.exe` (+ the rest of the runtime) | `virtio-dxg/qemu/build-runtime.sh`: the official runtime recipe plus the device |
| `initramfs-linux.img` | `virtio-dxg/guest/test/mkdevinitramfs.py`: the launcher's own initramfs plus `dxgkrnl_virtio.ko` and its boot hook |
| `payload\mesa-virtio-dxg.tar` | `virtio-dxg/guest/mesa/build-in-guest.sh` |

NVIDIA's user-mode drivers are not copied. The guest reads them read-only
from this machine (`System32\lxss\lib`, `Program Files\WSL\lib`, the
driver store), so a driver update on Windows reaches the guest at its next
boot. If anything is missing, the launcher names it and stops.

The boot hook writes nothing to the guest's disk. The environment and the
Mesa build go under `/run` (tmpfs), so a later launch without `-render dxg`
boots exactly as before. `-disk-snapshot` (any mode) sends the guest's disk
writes to a temporary overlay that QEMU discards when the VM stops.

### Stability work

The desktop and the test suite exposed four faults, each now fixed and
covered by a test:

| Fault | Effect | Fix |
| --- | --- | --- |
| IO-space views freed while flatviews pending RCU destruction still pointed at them | QEMU crashed within seconds of desktop activity | Views are QOM objects owning their region, freed by the last flatview |
| `dxghost` mapped and unmapped guest memory, and closed event waits, while holding its lock; inline requests hold the BQL and take that lock | QEMU deadlocked (ABBA, and a three-thread cycle through event callbacks) | Lock order: nothing enters the VMM under `h->lock`; such work is deferred to the unlocking thread. `executor_test` fails any VMM callback made under the lock (it does on the old code) |
| The device had no MSI-X vectors, so completions went through a shared INTx line | An ISR-read exit and an IOAPIC EOI per completion; under WHPX completions were lost and processes hung unkillably | Three MSI-X vectors (configuration, completions, events). Guest waits are killable and reap completions themselves after 100 ms, logging any that came without an interrupt |
| QMP virtio introspection asserted on the device ID | One `x-query-virtio-status` killed the VM | The device is listed as featureless |

## Next milestones

1. **Build the device** with the winq-emu runtime recipe:
   `python virtio-dxg/qemu/prepare.py <qemu-source>` after
   `experimental/qemu/prepare_qemu.py`, then the existing MSYS2 build.
2. **First hardware bring-up** (needs the owner's go-ahead because of the
   [November 3 display incident](HOST-DISPLAY-INCIDENT-2026-11-03.md)):
   load `dxgkrnl_virtio.ko`, then `nvidia-smi`, CUDA `deviceQuery`, a D3D12
   triangle, `vainfo`, `vulkaninfo` (Dozen).
3. **Guest staging**: ship the module with the launcher's kernel; expose the
   host driver store at `/usr/lib/wsl/drivers/<folder>` and the loader
   libraries at `/usr/lib/wsl/lib`, read-only, from the user's own install
   (never redistributed).
4. **Zero-copy presentation**: the compositor's frame as a shared D3D12
   resource, opened by QEMU's SDL frontend and drawn without copies. The
   upstream keyboard, cursor and window logic stay as they are.
5. **Per-process host workers**: one host process per guest GPU process with
   `WHvMapGpaRange2`, for separate GPU address spaces (CUDA unified
   addressing in several guest processes at once) and crash containment.
6. **Cross-process sharing and existing-sysmem allocations.**

## Security

The guest gets the same reach into the NVIDIA kernel driver as a Windows
process: escapes are forwarded. As with virtio-nvgpu and WSL2, the host GPU
driver is the trust boundary. `dxghost` makes sure the guest can only name
objects it created, cannot pass host pointers, cannot name host processes,
and keeps GPU virtual-address operations inside its own reservations.

## Building and testing

```sh
# Guest module against the exact Arch kernel (any x86_64 Linux, no root)
virtio-dxg/guest/check-build.sh 7.2.15.arch1-1
```

```powershell
# Host library and tests (MSVC; no GPU needed)
virtio-dxg/host/build.ps1
```

```sh
# Vulkan probe (MSYS2 UCRT64 gcc, Vulkan headers)
virtio-dxg/wine/build.sh <out-dir>
# Baseline on Windows itself
<out-dir>/nvvkprobe.exe --native
# In Linux with /dev/dxg (WSL2 or the VM), with Wine 11 and a prefix where:
#  - drive_c/windows/system32/DriverStore/FileRepository -> /usr/lib/wsl/drivers
#  - the DRS files are present (see "Going further" above)
gcc -shared -fPIC -O2 -o dxgkmt.so virtio-dxg/wine/dxgkmt_unix.c \
    -lgbm -lxcb -lxcb-dri3 -lxcb-present -lpthread
wine nvvkprobe.exe [--trace|--stacks]   # dxgkmt.so next to the .exe
```

### dxgvk: NVIDIA's Vulkan driver for Wine and Proton

`wine/dxgvk.dll` is a Vulkan driver (ICD) for Windows programs under Wine:
it loads NVIDIA's own Windows Vulkan driver (`nvoglv64.dll`, from the
host's driver store) with its kernel calls routed to `/dev/dxg`, and hands
the application the driver's objects and entry points unchanged. Every
Vulkan call runs in NVIDIA's driver: Vulkan 1.4, ray tracing, mesh shaders,
the vendor extensions. Only presentation is dxgvk's: a swapchain image is a
zero-copy window buffer (Mesa's shared GPU texture behind a virtio-gpu
dma-buf) imported into the NVIDIA device, presented to the Wine window's
X11 window with DRI3 and Present, which pass it to the compositor without
a copy. FIFO waits for the previous frame to be shown; mailbox and
immediate do not wait.

```sh
# build.sh also writes dxgvk.dll, dxgvk.json, vkpresent.exe and the
# Khronos loader (vulkan-1.dll), which programs must load natively:
WINEDLLOVERRIDES=vulkan-1=n
# The Windows loader finds the driver in the prefix's registry:
wine reg add 'HKLM\SOFTWARE\Khronos\Vulkan\Drivers' /v 'Z:\path\dxgvk.json' /t REG_DWORD /d 0 /f
DXGVK_DEBUG=1 wine vkpresent.exe 10 [fifo|mailbox|immediate]
```

Two things made NVIDIA's Windows driver accept a texture made in the VM:
the guest kernel now keeps the private data the kernel-mode driver
completed at creation (the allocation's layout) for later openers, and NT
handles of shared objects are descriptors the bridge marks for Windows code
(`kmt_bridge_fd_handle`). `d3d12vkimport.c` checks the same import on
Windows itself.

### Vulkan 1.3 for Linux applications

Native Linux Vulkan applications use Mesa's Dozen, which runs Vulkan on the
host's D3D12 driver in the same process: no translation into another
process or protocol. Dozen now reports Vulkan 1.3 with every required
feature; the design is in
[guest/mesa](../virtio-dxg/guest/mesa/README.md#vulkan-13-dozen) and the
checks in `bench/vulkan13`:

```sh
cd virtio-dxg/bench/vulkan13
glslc edstest.vert -o vert.spv && glslc edstest.frag -o frag.spv
for s in inline dirty zero; do glslc --target-env=vulkan1.3 $s.comp -o $s.spv; done
gcc -O2 -o edstest edstest.c -lvulkan && ./edstest      # 27 pixel checks
gcc -O2 -o vk13test vk13test.c -lvulkan && ./vk13test   # 1.3 features
```
