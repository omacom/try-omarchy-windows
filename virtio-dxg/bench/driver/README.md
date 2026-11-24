# Driver-level measurements

Tools for what one GL operation costs on the guest's graphics stack (GL →
Mesa's d3d12 driver → D3D12 → NVIDIA's user-mode driver → virtio-dxg).

- `passbench.c`: GTK 4's pattern for a CSS drop shadow, N times per frame
  (two new 96x96 textures and framebuffers, two clears, four draws, then
  everything deleted), with the CPU time per frame and per shadow and the
  GPU time (time-elapsed query). Surfaceless EGL:
  `gcc -O2 passbench.c -lEGL -lGL; ./a.out 50 300 0` (`REUSE=1` keeps the
  textures, isolating creation).
- `d3d12bench.cpp`: the D3D12 calls Mesa makes per offscreen pass (views,
  barriers, render-target binding, clears, descriptor copies) timed one by
  one, built the same way for native Windows (MSYS2:
  `g++ -O2 d3d12bench.cpp -ld3d12 -ldxgi`) and for Linux guests or WSL2
  (`g++ -O2 -I$DXH/include -I$DXH/include/wsl/stubs -I$DXH/include/directx
  d3d12bench.cpp $DXH/src/dxguids.cpp -ldl`, `DXH` = DirectX-Headers).
- `d3d12count_gen.py`: generates an `LD_PRELOAD` library counting every
  device, command-list, command-queue and fence call of a process by vtable
  slot (Mesa creates its device through `D3D12GetInterface`, which it
  wraps), plus repeats of the same pipeline state, root signature, vertex
  buffer, viewport and scissor. `kill -USR1` writes the counts since the
  previous signal. With `D3D12COUNT_TIME=1` every call is timed (TSC around
  it, the return address on a per-thread shadow stack) and so is every dxg
  ioctl, by number and by the D3D12 method it ran in, with the longest.

## Findings (RTX 5090 Laptop GPU, driver 621.24)

Per call, the same GPU and driver version:

| D3D12 call | Windows (native) | This VM | WSL2 |
| --- | --- | --- | --- |
| CreateRenderTargetView | ~300 ns | ~320 ns | ~350 ns |
| CreateShaderResourceView | 110-150 ns | ~300 ns | ~300 ns |
| ResourceBarrier | 140-190 ns | ~525 ns | ~540 ns |
| OMSetRenderTargets | 70-120 ns | ~460 ns | ~470 ns |
| ClearRenderTargetView | 180-270 ns | ~1,080 ns | ~1,130 ns |
| Descriptor writes and copies | ~27 ns | ~27 ns | ~30 ns |

NVIDIA's Linux (WSL) driver takes 3-6x longer per command-list call than
its Windows build; WSL2 measures the same as this VM, so it is the
driver, not the virtualization. Calls saved are time saved: a GTK shadow
pass costs ~35 us of CPU, against ~4.5 us of GPU time.

What GTK 4's renderer sends per draw while Files scrolls (`d3d12count`):
about 8 D3D12 calls, every draw with a new pipeline state, vertex buffer
and root constants, 85% with a new root signature (GTK changes shader and
vertex layout from one draw to the next), 3.6 descriptor-table bindings,
and per frame about 120 new textures (two per icon shadow).

Texture creation. A texture the recycling cache cannot supply (a new size,
as for items partly visible while scrolling) was a committed resource:
kernel allocation, GPU virtual address, residency, destruction, each a host
round trip, sometimes for milliseconds. `passbench 50 600 0 2` (two new
sizes per frame, as Files): frame p50 2.5-2.65 ms, p90 5-14 ms, p99 19-36
ms committed; p50 1.9 ms, p90 2.3 ms, p99 3.1-3.3 ms with textures placed
in pooled heaps. Files (touchpad-style scrolling, windowed VM): 118-131 fps
before, 130-148 fps after, no corrupted icons in 444.

Timed in Files while it scrolls (`D3D12COUNT_TIME=1`, ~510 frames), the
D3D12 calls take ~0.94 ms per frame: ClearRenderTargetView 2.1 us (72 per
frame), ResourceBarrier 1.0 us (148), SetPipelineState 0.5 us (180),
DrawInstanced 0.45 us (177), and per submission ExecuteCommandLists ~100+
us and Signal ~30 us. Of those, the submit and signal ioctls are 53-115 us
and 24-31 us in the guest against 15 us and 7.5 us of host D3DKMT time
(`DXGHOST_STATS=1`): the rest is the VM exit and QEMU's handling on the
vCPU thread. Every GTK frame was two submissions: GTK's `glFenceSync`
(which Mesa only defers when a context shares nothing) and the swap, whose
batch held only a barrier to COMMON for the window buffer. Window buffers
are simultaneous-access textures, which D3D12 returns to COMMON at the end
of every ExecuteCommandLists from any state; with that in the state
tracker the swap has nothing to submit and waits on the fence flush's
batch: one submission per frame.

NVIDIA's compiler takes 15-35 ms per pipeline and its Linux driver keeps
no compiled pipeline across processes. Mesa now records each pipeline's
description per executable (`~/.cache/mesa_shader_cache/d3d12_pso`) and
rebuilds them on worker threads when the program starts again: Files' 12-17
pipelines are ready ~110 ms after start, before it needs the first, where
it had spent 180 ms compiling them on its main thread.

The first time a GTK menu opened, its frame spent 29 ms in 36
`CreateDescriptorHeap` calls (0.8 ms each, most of it the `LOCK2` that maps
a shader-visible heap): GTK 4 gives each popup surface a GL context of its
own, and each d3d12 context created the heaps of all 8 of its batches.
Batches now take them from a pool when they first run; a context is 0.1-1
ms. That first menu frame stays at ~55 ms, the rest GTK building the menu
(13 ms when it opens again).

Measure in a VM with a window (`DISPLAY_OPT="sdl,gl=on"`). Headless, the
compositor's round trip took 2-4 ms instead of 0.7-1 ms, and the GPU stayed
at P4 (~1.4 GHz) even when fully busy, where a windowed VM reaches P0.
