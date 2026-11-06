# November 6 source and graphics-driver update

The development branch includes upstream master through
`5c7ad83a09241847c27fa1657b3af286cce2f5ed`. Merge
`5e0f33de1999f68cdf3c9125fe4eda03898fb1b6` passed the general, Mesa source,
native PR and native push CI runs. This resolves the missing upstream commits
observed during this sync. Future upstream commits still need their own sync.
The graphics backend and official Windows keyboard/mouse routing were preserved;
master was not changed. The upstream shared-file feature defaults to no approvals.

The user updated the Windows NVIDIA driver. Static service-file metadata now
reports **32.0.16.2114**. Matching `libnvwgf2umx.so` and
`libnvidia-gpucomp.so` have been copied to private staging, with hashes and the
new DriverStore package name recorded privately. No graphics library was loaded
and the actively loaded driver version was not queried. These two graphics
libraries are not yet assembled into a new guest image. Other NVIDIA APIs and
runtime dependencies still need their own integration and validation.

This backend uses Windows-provided Linux graphics UMDs with the experimental
D3D12 bridge. Installing a standard Linux NVIDIA kernel driver in the guest
would not implement this path; it does not currently have a passed-through
physical NVIDIA device.

A separate [Mesa 26.2.6 source recipe](../experimental/windows-native-gpu/mesa/recipes/26.2.6/README.md)
is prepared from the official release and ports all eleven existing patches.
It preserves newer upstream fence/residency/reclaim behavior. The first remote
batch at `5a33343` compiled the full Dozen profile and passed seven recipe and
eleven ICD/file controls. Its graphics profile stopped before compilation on a
malformed EOF diff hunk. That first candidate workflow failed; the successful
Dozen job is recorded separately.
[Exact receipt](evidence/mesa-2626-dozen-source-build-2026-11-06.json).
The corrected batch at `f95e3b5` compiled the full five-library graphics graph
and passed seven recipe, twenty actual-fence CPU and fourteen loader-alias
controls. It stopped at the strict `libgallium` API/ABI baseline gate. The Dozen
job passed again. The subsequent observation batch at `98aa0cf` confirms all
527 export names and their types/bindings/visibility/default flags match, with
only 83 gallium SONAME-based version tags changed. The new contract records
that exact transition and pins the dependency lists. Its acceptance build
requires approval. [ABI review and limits](MESA-2626-ABI-TRANSITION.md).
[Graphics result and remaining acceptance](evidence/mesa-2626-graphics-abi-gate-2026-11-06.json).
The existing 26.0.8 recipe and historical evidence remain intact. This source
update does not silently replace a private guest runtime or reinterpret old tests.

Remaining steps are a successful build under the reviewed ABI/dependency contract,
the failure-regression matrix on this version, a separately approved
data-only guest-image assembly, then separately approved hardware integration.
The host driver update invalidates assumptions about the previous hardware
baseline; it does not prove that the display incident cannot recur.

Readiness remains **backend 4/8, desktop 0/4, diagnostic apps 3/3**. Normal session
flow, preserved end-to-end Windows input, responsive browsing and multiple
windows through 4K, sustained sessions, bounded resources, cleanup/recovery and
near-native performance remain unverified. CUDA, complete OpenGL/Vulkan support,
video acceleration, DLSS and hardware ray tracing remain requirements. No
daily-use or stable-backend notification is warranted.
