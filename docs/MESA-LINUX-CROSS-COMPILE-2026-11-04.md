# Linux Mesa compilation with the upload guards

**November 4, 2026:** the full Linux-target Mesa 26.0.8 D3D12, EGL and GBM
source builds from Windows using native CPU tools. Both the uploader ownership
fix and constant-buffer failure guard are included, together with the captured
owned-presentation overlay. Six Linux shared libraries link. Data-only ELF
inspection preserves all **527 API exports** compared across five captured
libraries, including symbol type, binding, visibility and version.

This verifies compilation and a bounded binary-interface comparison. It does
not verify library loading, rendering, the Quickshell crash fix or daily use.
No VM, GPU workload, WSL session, window or desktop input is started. The
installed runtime and VM image remain unchanged. Hardware testing remains
[on hold](HOST-DISPLAY-INCIDENT-2026-11-03.md).

## Source and build scope

The source is reconstructed from the pinned Mesa archive, public graphics
patches 1–4 and the exact private owned-presentation patch. The changed paths
match the captured source associated with run 10's installed Gallium module
(`0128e184…`). The public uploader and constant-buffer guards are then applied
and their source hashes verified. Both complete affected files compile in the
full graph; this does not use the CPU callback substitutes.

The profile includes Wayland, EGL, GBM, D3D12 Gallium, shader caching, zlib and
zstd, with captured Wayland 1.26.0, protocols 1.47 and libdrm 2.4.131 dependencies.
GLX, LLVM, Vulkan and video drivers are disabled. Unlike the captured build,
this configuration also enables GLES1 through its default option. The compiler
and linker differ from that Linux build; this is not an identical release build.

Zig 0.14.1 targets `x86_64-linux-gnu.2.38`, using actual GCC C++ headers and GNU
shared runtimes rather than Zig's bundled libc++. Meson 1.8.3 generates the
build graph. A sequential executor runs the reviewed native compiler, parser
and Python generator commands with hidden child consoles, two logical CPUs,
below-normal priority and a **2 GiB job commit limit**. Peak observed build-job
commit is **1,378,672,640 bytes**. This is a compiler-resource observation,
not a desktop-resource measurement.

All **1,003 unique commands** in the selected default graph complete. The
executor materializes 866 response files from Ninja's actual expanded metadata;
it does not launch a Ninja build. Previously completed objects are retained
when the failed step is corrected. All Linux outputs remain data files.

Four Wayland protocol C/header outputs are reused from the captured build,
with exact XML, generator version, options and output hashes checked. This
covers linux-dmabuf and presentation-time. Independent protocol generation is
not verified. Ten unused protocol targets have no captured outputs and are
explicitly rejected if requested. Other selected source generation runs using
native CPU tools.

## Link checks and retained failures

Initial attempts expose C++ header search ordering, Meson's handling of split
include flags, Windows-only WinFlex output and version-script argument
forwarding. Each failed attempt remains in the private build journal. Corrected
build commands preserve the source and original graph metadata; the actual
adaptations are recorded in the receipt.

The first linked files contain Windows build RUNPATHs. Their Gallium exports
also lack the captured GNU default symbol-version metadata. A separate relink
uses the same completed objects, removes build RUNPATHs and names Gallium's
existing export-list node with its SONAME. This reproduces the captured
[GNU default-symbol-version convention](https://sourceware.org/binutils/docs/ld/Options.html).
The earlier outputs and failed comparison remain retained. The final comparison
checks symbol versions as well as exported names; the synthetic zero-sized
absolute version-node symbol is accounted for separately from API exports.

| Linked library | API exports | Captured API comparison |
| --- | ---: | --- |
| Gallium 26.0.8 | 83 | All match, including versions |
| GLES2 | 358 | All match |
| GBM | 38 | All match |
| EGL | 47 | All match |
| DRI GBM backend | 1 | All match |
| GLES1 | 145 | No captured counterpart |

All six final files are x86-64 Linux ELF shared libraries with no RPATH or
RUNPATH. Their dependency names, symbol-version requirements and hashes are
recorded without invoking a loader. This does not establish complete ABI or
runtime compatibility.

## Remaining limits

These artifacts retain build-prefix defaults and are **not a guest installation
package**. A proper Linux build and packaging pass still needs validation.
A separate [remote Linux compile-only run](https://github.com/7Wdev/try-omarchy-windows/actions/runs/41855718282)
now passes for PR head `545d12b` (workflow merge commit `5902aaf`). Its recorded
source hashes match both upload guards, and its linked Gallium/EGL hashes are
retained in the [CI receipt](evidence/mesa-linux-source-ci-2026-11-04.json).
That tested workflow revision builds patches 1-4 and 9-10 with `platforms=[]`; it does
not include the private presentation overlay or reproduce this Wayland
source/dependency profile. It never loads the libraries.

The [owned-presentation source follow-up](MESA-OWNED-PRESENTATION-SOURCE.md)
now adds the captured presentation patch to the public recipe and extends the
workflow to pinned Wayland and Linux staging. The [completed Linux staging run](evidence/mesa-wayland-stage-ci-2026-11-04.json)
now verifies that component layout and its interfaces without loading a library.
The historical cross-build results above retain their original scope.

The patched runtime has not been installed or loaded. Quickshell recovery,
normal launcher sessions, ordinary Windows input, sustained browsing and app
windows through 4K, bounded runtime resources and cleanup/recovery all remain
open. CUDA, Vulkan, video, DLSS, ray tracing and the requested matched-native
performance are outside this compile check. Counts remain **backend 4/8,
desktop 0/4, diagnostic scene 3/3**.

[Structured build evidence, source/object/library hashes and exact limits](evidence/mesa-linux-cross-compile-2026-11-04.json).
