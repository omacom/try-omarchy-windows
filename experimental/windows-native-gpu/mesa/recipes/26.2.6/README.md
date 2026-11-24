# Mesa 26.2.6 source candidate

This recipe ports the eleven existing experimental bridge and failure guards to
[Mesa 26.2.6](https://docs.mesa3d.org/relnotes/26.2.6.html), released October 28,
2026. The official archive SHA-256 is pinned in `recipe.json`. The historical
26.0.8 recipe remains separate and remains the default of the existing tools.

The [first remote batch](https://github.com/7Wdev/try-omarchy-windows/actions/runs/42206143342)
compiled the complete Dozen profile and passed seven recipe and eleven ICD/file
controls at commit `5a33343`. The graphics profile stopped during patch dry-run:
the generated diff omitted a marker for an upstream file without a final newline.
That first overall run failed before graphics compilation or fence execution.
[Dozen receipt and explicit limits](../../../../../docs/evidence/mesa-2626-dozen-source-build-2026-11-06.json).
The corrected batch at `f95e3b5` compiled all five graphics libraries and passed
seven recipe, twenty actual-fence CPU and fourteen loader-alias cases. It then
failed the strict 26.0.8 ABI baseline comparison for `libgallium-26.2.6.so`.
No accepted graphics archive was produced. [Exact scope and failure](../../../../../docs/evidence/mesa-2626-graphics-abi-gate-2026-11-06.json).
The diagnostic batch at `98aa0cf` saves symbol/type/binding/version observations
before the unchanged gate. Reviewing its completed artifact confirms all 527
names/types/bindings/visibility/default-version flags are preserved; only the
83 gallium SONAME-based version tags change. The contract now records that exact
independently derived transition and pins the five dependency lists. Its new
acceptance build still needs approval. [Review and limits](../../../../../docs/MESA-2626-ABI-TRANSITION.md).
This is not an installed guest update or a usable desktop. It contains no NVIDIA
binary, guest disk, host driver installer or GPU test.

The graphics profile preserves timeline/native sync files, native images,
render-node discovery, owned presentation, uploader ownership and constant-buffer
failure guards. It also preserves upstream's new lazy event registration,
device-15 residency initialization, pending-free list and allocation reclaim
loop. Native resource declarations are renewed and cancelled on each allocation
attempt. Raw fence waits reject the device-removal sentinel and recheck actual
completion after an event wake or blocking wait.

The Dozen profile ports FD export/import and native-buffer admission separately.
Its ICD manifest retains the version selected by the upstream build source.
An advertised API version is not conformance, functional Vulkan presentation,
ray tracing, DLSS, CUDA or video-acceleration evidence.

The candidate workflow has two GitHub-hosted Linux CPU-build profiles. It runs
source-recipe rejection controls and compiles the full selected graphics/Dozen
graphs with two compiler jobs per runner and a 30-minute job deadline. Graphics
controls compile the actual upstream and patched fence functions against CPU
callbacks; twenty patched cases must pass and the upstream sentinel defect must
be reproduced. The resource retry check verifies source scope; it does not
exercise real memory pressure or GPU allocation retries.

Staging inspection reads ELF and loader metadata without loading the libraries.
The graphics ABI contract requires the reviewed public export metadata and the
exact 83 gallium version-tag transition, with its SONAME updated to 26.2.6.
Other ABI/dependency changes fail the gate and need explicit review. The
527-export count remains unchanged. Generated protocol hashes
and EGL/GBM/GLES loader aliases are also checked. Dozen has separate file/ICD
rejection controls. Successful archives remain inactive build components.

After an approved successful build, a separate data-only guest-image update must
handle the new gallium filename, its dependencies, and the private NVIDIA UMD
package matching the updated host. The old candidate builders and recorded
26.0.8 evidence do not establish this new image's compatibility. Guest assembly,
the full ownership/failure regression matrix on the new version, GPU rendering,
normal session operation, hardware cleanup/recovery and performance remain open.
Local hardware testing requires separate approval because this PC previously
experienced host-wide display corruption; an inactive desktop does not isolate
the physical GPU or its driver.
