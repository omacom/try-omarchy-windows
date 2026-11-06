# Mesa 26.2.6 source candidate

This recipe ports the eleven existing experimental bridge and failure guards to
[Mesa 26.2.6](https://docs.mesa3d.org/relnotes/26.2.6.html), released October 28,
2026. The official archive SHA-256 is pinned in `recipe.json`. The historical
26.0.8 recipe remains separate and remains the default of the existing tools.

This is an **unbuilt source candidate** until a completed run for this revision
establishes otherwise. It is not an installed guest update or a usable desktop.
It contains no NVIDIA binary, guest disk, host driver installer or GPU test.

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
The graphics ABI contract initially requires the exact historical public export
types, bindings, visibility and versions, with the gallium SONAME updated to
26.2.6. A changed upstream ABI fails the gate and needs explicit review; this
recipe does not invent new successful export counts. Generated protocol hashes
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
