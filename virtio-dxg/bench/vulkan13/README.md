# Vulkan checks for Dozen

Small programs that check, with rendered pixels and read-back buffers, what
the guest's Vulkan driver (Mesa's Dozen on NVIDIA's D3D12 driver) offers.
Each prints one `ok`/`FAIL` line per case and a summary. Build and run them
in the guest (`glslc` is in `shaderc`):

```sh
glslc edstest.vert -o vert.spv && glslc edstest.frag -o frag.spv
for s in inline dirty zero; do glslc --target-env=vulkan1.3 $s.comp -o $s.spv; done
for s in bl.vert bl.frag blint.frag dual.frag uint.frag vpvs.vert vpgs.vert vpgs.geom \
         samp.vert samp.frag half.frag; do
    glslc --target-env=vulkan1.3 $s -o $s.spv
done
for t in edstest vk13test bltest samptest srgbtest; do gcc -O2 -o $t $t.c -lvulkan; done
./edstest && ./vk13test && NOBINDLESS=1 ./vk13test
./bltest; ./samptest && BINDLESS=1 ./samptest && ./srgbtest
```

| Program | Checks | November 14, 2026 payload |
| --- | --- | --- |
| `edstest` | Extended dynamic state 1 and 2: every state as dynamic, against its pipeline-baked equivalent | 27/27 |
| `vk13test` | Inline uniform blocks, zero-initialized workgroup memory, maintenance4, format properties 3, in both descriptor modes (`NOBINDLESS=1`) | pass |
| `bltest` | What Blender 5.2's Vulkan backend requires: dual-source blending, logic ops, viewports chosen by the vertex and geometry shader, last-vertex provoking | 21/26: no viewport index from geometry shaders; odd strip triangles in last-vertex mode take the wrong vertex |
| `samptest` | Sampling and rendering sRGB views (decode, encode), FP16 targets, both descriptor modes (`BINDLESS=1`) | pass |
| `srgbtest` | sRGB conversions in blits; raw bytes in copies | pass |
