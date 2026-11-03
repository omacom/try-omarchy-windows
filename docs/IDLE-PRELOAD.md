# Ordinary preloaded processes leave the GPU bridge idle

**November 3, 2026:** ordinary `close` and three-argument `ioctl` calls no longer
construct the GPU bridge when that process has never used it. Real preloaded
process controls pass, along with **217 existing fake-worker controls**. The
fixed shim is also used in [run 10](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md),
whose captured terminal prompt has no zero-activity summary flood. Quickshell
and shutdown still fail; daily-use acceptance and performance remain open.

The run-9 terminal capture contains repeated zero-activity bridge summaries from
ordinary shell commands that inherit `LD_PRELOAD`. Both generic FD hooks called
the lazy bridge factory unconditionally, allocating state, taking its mutex and
registering an exit reporter even for processes that never opened `/dev/dxg`.

The hooks now look up already-published state and use the existing raw syscall
path when no bridge exists. Explicit GPU opens and bridge hooks still construct
state through the existing factory. Atomic acquire/release publication lets an
ordinary FD call safely overlap the first explicit GPU open. Once state exists,
its ownership checks, mutex, shutdown flag and finalization remain in place.
GPU failure and active-process diagnostic output are retained.

The unchanged bridge first reproduces the idle-output failure. The fixed
production-profile library then passes actual `LD_PRELOAD` subprocess controls
with multithreaded `/dev/null` closes, real pipe ioctls, descriptor balance,
preserved errno, a deliberately failed explicit GPU open, and eight repeated
concurrent first-open cases. Every backend selector is removed, so those controls
cannot select a native worker or GPU endpoint. A separate strict default-profile
build passes all 217 existing fake-worker transport controls, including error
preservation and live descriptors during late DSO finalization. CI also runs the
new controls against its 4K library build.

These controls verify this forwarding and reporting change. They do not measure
desktop latency, prove general libc interposer compatibility, or resolve the
QEMU retirement and Quickshell draw-time failures. The existing three-argument
ioctl scope is unchanged. Full healthy desktop operation, sustained browsing,
recovery, broad APIs and at least 93% matched-native performance remain open.
Counts stay **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

[Exact source and CPU evidence](evidence/idle-preload-2026-11-03.json) ·
[Run 9 capture and retained failures](NORMAL-SESSION-GPU-ATTEMPT-2026-11-03.md).
