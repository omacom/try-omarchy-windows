# Normal-session bootstrap candidate

**Original pre-boot snapshot, November 2, 2026:** the owned host runtime can
explicitly select the boot console, and a separate normal guest bootstrap
compiles. A matched private image is assembled and its complete archive is
verified. Subsequent [actual normal-account boot attempts](NORMAL-SESSION-BOOT-ATTEMPTS.md)
reach the official login and broker. Later [frame-transport tests](NORMAL-SESSION-FIRST-GPU-FRAMES.md)
submit 61 native 4K frames, with an incomplete scene and failed shutdown.
No complete normal desktop passes. Counts stay **backend 4/8, desktop 0/4,
scene 3/3**.

## Host selection

`--driver-boot-console` requires the persistent original launcher session, its
inherited lifetime capability and SDL coordinator. Invalid and duplicate use
fails before runtime creation. The selection travels through the owned runtime
and original profile composer to `BootDisplay::StandardVga`. The default remains
`None`; the profile JSON schema remains version 1.

Both strict Windows configurations pass. Six real Go profiles cover accepted and
rejected inputs, cursor preferences and Unicode. Both composition modes preserve
the original input devices, SDL settings and QMP roles. The complete prepared Go
arguments reach lifetime validation with the new option, where a deliberately
non-inherited capability rejects startup. Twenty-two lifetime/boot-option CLI
controls pass in each build. These controls start no VM or GPU device.

## Guest startup implementation

The existing PID1 source has an explicit `WDDM_NORMAL_SESSION_BOOTSTRAP` build
mode. Its default preprocessed code and compiled static binary remain exactly
equal to the pinned previous bootstrap under the same compiler and flags. All
three compiled variants refuse ordinary host execution before any mount call.
Sixteen pure command-line controls cover the explicit opt-in and malformed or
conflicting values.

The new mode requires its two payload directories and exactly one
`tryomarchy.nvgpu=1` kernel option. It retains the read-only source block device
check and 512 MiB temporary root overlay. It binds the runtime read-only rather
than copying hundreds of MiB into that overlay. The guest's own coreutils copy
the official factory/native overlay with Unix modes and symlinks preserved,
inside the new root. That setup operation has a 30-second startup deadline.
The temporary overlay-source mount is then removed and normal systemd starts.

This mode installs the ordinary provisioning path and normal session service.
It does not create an account, substitute a PAM session, mask owner provisioning
or install a finite desktop workload. The root activation service follows the
account selected by provisioning. The inner desktop entry keeps its desktop ID
and official watchdog while selecting the compositor exec wrapper.

Mounting, copying the overlay and running systemd have **not been executed by
these CPU controls**. The code and archive are candidates for the next guest
integration run.

## Matched private package

The candidate includes production service/preparer/wrapper binaries, the
persistent hook, installed Aquamarine library, matched broker, kernel modules,
Mesa/interop dependencies and local vendor runtime. It uses committed bytes and
Git Unix modes from the pinned official overlay tree, independently of local
checkout mode changes. The installed Aquamarine library has no build-directory
RPATH. The corrected QEMU artifact's graphics source bytes match exactly.

The archive check verifies **403 payload entries including 23 symlinks**, plus
**68 directories**, every payload hash and mode, root ownership and gzip integrity. The
runtime is about **310 MiB**, the configuration overlay **1.74 MiB**, and the
compressed image **116.1 MiB**. The old finite startup hook, test services and
probe payloads are excluded. The first assembly's diagnostic GTK library was
rejected by that exclusion check; the failed attempt is retained.

The image and proprietary libraries remain private and are not redistributed.
No source disk was modified or guest launched during assembly.

Next: verify a controlled normal boot using the owned host and matched package,
then fix any actual startup or application-environment failures. A complete GPU
desktop, responsive ordinary input/browsing, sustained use, resource bounds and
cleanup/recovery need end-to-end evidence. Broader CUDA/OpenGL/Vulkan/video,
DLSS, hardware ray tracing and **at least 93% matched-native performance** remain
unverified.

[Structured evidence](evidence/normal-session-bootstrap-2026-11-02.json) ·
[Root activation service](ROOT-SESSION-SERVICE.md) ·
[Native renderer selection](NATIVE-SESSION-RENDERER-SELECTION.md).
