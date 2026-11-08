# virtio-dxg: driver-level NVIDIA virtualization on a Windows host

Updated 2026-11-08. Code: [`virtio-dxg/`](../virtio-dxg).

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
translator on it would be reverse engineering against NVIDIA's EULA with no
stability. GPT's earlier investigation reached the same conclusion
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

## Design

```text
Guest (Arch, kernel 7.2)                     Host (Windows, QEMU/WHPX process)
──────────────────────────────────           ─────────────────────────────────
App / Hyprland
NVIDIA WSL user-mode driver (unmodified)
Mesa d3d12 / Dozen / d3d12 video (unmodified)
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

- **Presentation.** WSLg copies every frame to the CPU and sends it over RDP.
  This backend's target is zero-copy: the guest's frame stays a GPU resource
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

| Capability | How | Expectation |
| --- | --- | --- |
| CUDA | NVIDIA `libcuda` (WSL build) | Native kernels. WSL2 measures within a few percent for GPU-bound work. Launch-bound work pays the per-call cost. |
| NVENC / NVDEC | `libnvidia-encode`, `libnvcuvid` | Native engines. |
| D3D12, including DXR ray tracing | `libnvwgf2umx.so` | Native driver. Few Linux apps use D3D12 directly. |
| OptiX (RT cores from CUDA) | `nvoptix.bin` | Native. |
| OpenGL 4.6 | Mesa `d3d12` Gallium driver | Translation on the CPU side; GPU-bound work close to native, draw-call-heavy work less. |
| Vulkan | Mesa Dozen (Vulkan on D3D12) | Partial Vulkan 1.3. Fine for compositors and many apps; not a full desktop-game Vulkan. |
| VA-API video | Mesa `d3d12` video | H.264/HEVC/AV1 decode and encode on NVIDIA. |
| **DLSS in Vulkan/Proton games** | — | **Not reachable.** DLSS for Vulkan needs NVIDIA's native Linux Vulkan driver, which talks RM ioctls, not dxgkrnl. NVIDIA ships no such driver for this interface. NGX exists here for D3D12 only. |
| **Vulkan hardware ray tracing** | — | **Not reachable through Dozen.** Ray tracing is available through D3D12 (DXR) and OptiX. |

A flat "93% of native for everything" is therefore not achievable on a
Windows host by anyone outside NVIDIA today. CUDA, encode/decode and D3D12 can
approach native; OpenGL and Vulkan go through translation layers, and the two
items in bold need a driver NVIDIA does not publish. The only way to get the
real NVIDIA Linux Vulkan driver into a VM is to give the VM the GPU's RM
interface: VFIO on a Linux host (virtio-nvgpu), or device assignment, which
Windows client editions do not offer.

## Status

| Piece | State |
| --- | --- |
| Wire contract `protocol/virtio_dxg.h` | Done |
| Guest: `dxgkrnl` over virtio (`guest/`) | Done. Builds against the installed guest kernel `7.2.9-arch1-1`, no warnings in new code at `W=1`. Not yet loaded in a VM. |
| Host: `dxghost` executor (`host/`) | Done for 50 commands. Unit tested against a fake driver; struct layouts proven identical to the Windows SDK and between GCC and MSVC. Not yet run against the real driver. |
| QEMU device `virtio-dxg-pci` (`qemu/`) | Written, **not yet compiled**; needs the winq-emu build (MSYS2/CI). |
| Launcher option, guest image staging | Not started |
| Desktop presentation (zero-copy scanout) | Not started |

Not offered yet, with exact replies: cross-process sharing (NT handles,
`OPENRESOURCE`), existing-sysmem allocations (`VK_EXT_external_memory_host`),
standard allocations, statistics, optional dxgkrnl features.

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
virtio-dxg/guest/check-build.sh 7.2.9.arch1-1
```

```powershell
# Host library and tests (MSVC; no GPU needed)
virtio-dxg/host/build.ps1
```
