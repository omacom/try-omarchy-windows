# virtio-dxg Mesa 26.2.6 candidate

This branch combines Claude's KMS mirrors and compositor zero-copy scanout with
our resource-ownership, upload, constant-buffer, fence and Dozen FD guards.
It uses the real guest dxg kernel ABI, not the older private userspace broker.
The first cohort at d717dfa compiled completely and its CPU controls passed,
but runtime-archive acceptance failed because its receipt also listed SDK
headers not shipped in the tar. The corrected receipt and an additional
submitted-fence guard are **prepared but untested**. No GPU/VM run occurred. See
[status and performance limits](../../../docs/VIRTIO-DXG-INTEGRATION.md).

The versioned recipe in `versions/26.2.6/` pins the official source archive,
DirectX-Headers commit, complete combined diff and before/after source hashes.
Source `VERSION` remains 26.2.6. The 26.2.4 deadlock backports in historical
`0003`/`0004` are already upstream and are not applied twice. The older numbered
patches are retained as source history; the build uses only the pinned recipe.

The new recipe also tracks mirror writes per context, retains the scanout owner
through borrowed wrappers and rejects zero-ref weak lookups. Up to 16 pending
mirrors avoid a per-flush heap list. A failed GPU completion cannot publish a
CPU readback. Application windows still use shared-memory readback/upload;
compositor scanout zero-copy does not make applications zero-copy.

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
