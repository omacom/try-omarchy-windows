# Owned presentation source and Wayland build

**November 4, 2026:** the opt-in Mesa owned-presentation patch is included in
the public recipe as `0011-linux-d3d12-owned-presentation.patch`. Its bytes and
four resulting source hashes match the previously captured presentation patch.
It is the same source included in the [complete CPU-only cross-build](MESA-LINUX-CROSS-COMPILE-2026-11-04.md),
with both uploader failure guards. Publication does not establish a new rendered
frame or resolve the [host display incident](HOST-DISPLAY-INCIDENT-2026-11-03.md).

**Review - November 5, 2026:** the [completed Linux run for `6c652f3`](https://github.com/7Wdev/try-omarchy-windows/actions/runs/41867101381)
passes the full public Wayland/D3D12 build and component staging checks. It
verifies 20 patched source files, four independently generated protocol files,
five staged libraries, 527 API exports and the six intended loader-alias targets.
The eight source-recipe and fourteen alias controls also pass. No Mesa library
is loaded or executed. [Original CI receipt and exact limits](evidence/mesa-wayland-stage-ci-2026-11-04.json).

The previous public compile workflow omitted this patch and the Wayland
platform. The successful follow-up builds that desktop path, then stages and
inspects its Linux files. Earlier run 41863425648 builds and installs the
pinned Wayland libraries and scanner, then fails when protocol installation
cannot find an unbuilt generated enum header. The workflow now compiles those
protocol targets before installation. Mesa compilation and staging were not
reached in that failed run; it does not count as a staging pass.

## Resource behavior

The patch permits GBM scanout usage to select the existing owned, opaque native
allocation. It keeps the texture in GPU layout; scanout usage cannot silently
make an owned texture CPU-linear. It does not create a KMS plane or physical
scanout device. Linear allocation, foreign attachment, shader-image usage,
unsupported formats, mipmaps, arrays and multisampling retain their resource
restrictions.

The path requires both `D3D12_EXPERIMENTAL_NATIVE_BUFFERS` and
`D3D12_EXPERIMENTAL_NATIVE_PRESENTATION`, the existing native-buffer hooks and
a presentation-support response of exactly one. Defaults remain disabled. A
matching kernel, broker, shim and Windows-native runtime still provide resource
ownership and transport; these source changes do not supply a standalone GPU
installation. Official Windows keyboard and mouse code is unchanged.

## Recipe and CPU controls

Apply the four pinned graphics predecessors, then
`apply_mesa_native_presentation.py`, followed by the uploader and constant-buffer
failure guards. The presentation helper verifies all predecessor digests,
changed and unchanged source hashes, dry-runs with zero fuzz and verifies the
result. Repeated application verifies without rewriting.

Eight source controls pass on genuine copied Mesa source: normal application,
repeat verification, and six rejections for wrong version, modified predecessor,
modified target, mixed targets, corrupted patch and broken dependency chain.
All rejected trees remain unchanged. A process-sandbox denial of Git's MSYS
namespace during fixture preparation is retained; the hidden source-only
controls pass outside that restricted process environment. No Mesa runtime, VM, compositor or graphics API is invoked by these recipe
controls.

The new ELF reader is checked against five captured Mesa libraries and matches
all 527 API exports, including types, bindings, visibility and versions. This
validates the reader against known files; it does not test a new Linux staging
tree locally or prove complete binary compatibility.

## Loader-alias validation follow-up

The first alias gate checked that links existed and stayed inside the staging
tree. It accepted an EGL alias pointing to GBM inside that tree, while the
separate canonical library files could still pass their export checks. That
could let a broken loader layout pass verification.

The gate now requires each EGL, GBM and GLES2 alias to resolve to its intended
versioned library. Fourteen filesystem controls use copied captured Mesa files:
two valid-chain cases pass and twelve invalid cases are rejected. They cover
all six wrong-target aliases, escape, dangling link, cycle, missing alias,
regular-file replacement and an unlisted escaping link. Library contents and
the input tree remain unchanged. The completed Linux CI repeats these controls
with its newly staged component files, without loading or executing Mesa code.
[Reproduction and exact alias controls](evidence/mesa-stage-alias-controls-2026-11-04.json).

## Linux build and staging contract

The workflow pins [Wayland 1.26.0 and protocols 1.47](https://wayland.freedesktop.org/releases.html)
by archive hashes. Both archives were downloaded and verified as data; the two
protocol XML inputs match the captured inputs. Wayland libraries and scanner
build with tests and documentation disabled. The scanner performs XML source
generation; no Wayland server or desktop session starts.

Mesa builds with D3D12, Wayland, EGL and GBM, with GLES2 enabled and GLES1, GLX,
LLVM, Vulkan, video and XML configuration disabled. Its installation prefix is
`/usr`, with `libdir=lib`. Meson stages only into the owned CI destination using
`--no-rebuild --destdir`; no host runtime is installed. These components are
not a usable backend release.

The data-only stage verifier requires:

- All final hashes from the seven applied source manifests.
- Four freshly generated protocol C/header files matching the captured hashes,
  and the two Wayland queue/timeout code paths enabled during compilation.
- Exact configured Wayland client/server 1.26.0, protocols 1.47 and EGL-backend
  version 3 metadata.
- Five x86-64 ELF libraries preserving the 527 compared API exports and symbol
  versions, correct SONAMEs, and no RPATH/RUNPATH or host dependency paths.
- Expected development files and library aliases bound to the intended versioned
  libraries, with every staged symlink resolving inside the owned destination.

The completed `6c652f3` run uploads evidence and build logs. The workflow now
also defines a component archive containing the staged files, build receipt and
upstream notices. It preserves Linux symlinks and checks their targets after
relocation, then reads back packaged library hashes and link targets against
the build receipt without extracting or running them. This archive step is new
after the completed run cited above; that receipt does not verify packaging.
The archive supplies no installer, activation script, proprietary runtime or
VM image.

The workflow does not load the Mesa libraries, call a graphics API, start a VM
or test shaders. Its Ubuntu compiler
and other system dependency versions still differ from the captured guest.

Normal Omarchy startup, responsive browsing and multiple windows through 4K,
sustained sessions, resource bounds, cleanup/recovery, broader API support and
matched-native performance remain unverified. Hardware testing remains on hold.
Counts stay **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

[Source provenance and exact CPU controls](evidence/mesa-owned-presentation-source-2026-11-04.json).
