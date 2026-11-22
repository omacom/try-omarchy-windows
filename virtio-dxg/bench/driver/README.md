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
  `ID3D12Device` and `ID3D12GraphicsCommandList` call of a process by vtable
  slot (Mesa creates its device through `D3D12GetInterface`, which it
  wraps), plus repeats of the same pipeline state, root signature, vertex
  buffer, viewport and scissor. `kill -USR1` writes the counts since the
  previous signal.

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

Measure in a VM with a window (`DISPLAY_OPT="sdl,gl=on"`). Headless, the
compositor's round trip took 2-4 ms instead of 0.7-1 ms, and the GPU stayed
at P4 (~1.4 GHz) even when fully busy, where a windowed VM reaches P0.
