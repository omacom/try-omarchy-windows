# Normal-account GPU boot attempts

**First four attempts, November 2, 2026:** four background boots reach official provisioning, SDDM,
PAM, UWSM and the authenticated compositor broker. The corrected runtime creates
a native **3840×2160 swapchain**, but the first persistent output submission
fails before a frame reaches Windows. **No normal desktop frame passes.** Counts
remain **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

Later [normal-session GPU transport tests](NORMAL-SESSION-FIRST-GPU-FRAMES.md)
submit 61 native 4K frames after correcting these startup failures. Their capture
contains only background/cursor and their shutdown fails; full desktop and
cleanup acceptance remain open.

## Actual boot path

These runs use the real Go supervisor boundary, the explicit host boot-console
option, the matched QEMU binary and the separate normal guest bootstrap. The
official `tryomarchy.instant=1` path creates and finalizes the disposable account;
the original SDDM/PAM/UWSM/watchdog flow reaches the compositor exec wrapper.
The root service authenticates that actual compositor and starts its broker.
There is no forced-UID fixture or finite desktop service in these images.

The launcher GUI is not activated. Runtime paths, read-only raw storage, the
graphical bootstrap command line, firmware path and a disposable loopback SSH
forward are explicit test adaptations to an exported real launcher profile.
Original input arguments and QMP roles are preserved. The runs use an owned
inactive Windows desktop, send no host or guest input, and verify the held QEMU
process and kernel-reported QMP peer before diagnostics or shutdown. Initial
console journal forwarding makes these diagnostic runs unsuitable as performance
measurements.

## Retained failures and corrections

| Run | Actual native submissions | Result |
| --- | ---: | --- |
| 1 | 0 | GBM searched a build-machine path absent from the guest. |
| 2 | 2 | GBM loads after its path correction; EGL initialization still fails. |
| 3 | 11 | Correct Mesa opt-ins reach a native 4K swapchain; no frame is presented. |
| 4 | 11 | A diagnostic-only follow-up records `ENODEV` at persistent output submission; zero frames. |

The root-owned environment now supplies `GBM_BACKENDS_PATH` for the installed
runtime and the exact `D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE=true` and
`D3D12_EXPERIMENTAL_NATIVE_BUFFERS=true` settings consumed by the patched Mesa
sources. The wrapper and persistent hook require the matching interop settings.
The previous unused `D3D12_EXPERIMENTAL_NATIVE_SYNC` spelling is removed.

The hook also handles journald's actual stream socket with
`MSG_DONTWAIT | MSG_NOSIGNAL`. Its separate pipe/file path remains nonblocking
and preserves the parent's descriptor flags. Real readable, full and closed
socket controls verify message delivery, bounded failure and SIGPIPE handling.
The fourth image changes only that hook payload from the third image; rendering
and acceptance conditions are unchanged.

The final source passes **55 persistent-hook, 43 wrapper and 34 root-service
CPU controls** with strict production builds. Archive verification checks all
403 payloads, 68 directories, 23 symlinks, modes, root ownership and gzip
integrity. Proprietary runtime libraries and images remain private.

## Cleanup and remaining work

All four observed VMs exit naturally after authenticated QMP powerdown. Their
native owners exit zero, owned process jobs have zero remaining descendants,
selected native live-object counters and final aperture mappings are zero, and
reported cleanup failures are zero. Stopped compositor-group retirement reaches
zero before VM exit; no aperture mappings require release after VM exit. The
source disk is held against write/delete sharing and its full hash is unchanged
before/after every run. Installed NVIDIA runtime guards also remain unchanged.
These observations cover these failed startup paths; they do not certify general
crash recovery, relogin or sustained use.

Next is isolating EGL context/fence readiness at the first persistent commit.
`ENODEV` is observed; its precise failing operation is not yet isolated. The
earlier finite hook skipped its initial pre-render buffer, which is a useful
comparison, not proof that the new failure has the same cause. Physical input
latency, normal-session browsing and application windows, sustained use,
boot-console handoff, broad CUDA/OpenGL/Vulkan/video support, DLSS, hardware ray
tracing and **at least 93% matched-native performance** remain unverified.

[Structured evidence and exact source/artifact hashes](evidence/normal-session-boot-attempts-2026-11-02.json) ·
[Bootstrap implementation and original pre-boot controls](NORMAL-SESSION-BOOTSTRAP.md) ·
[Daily-use acceptance](DAILY-USE-READINESS.md).
