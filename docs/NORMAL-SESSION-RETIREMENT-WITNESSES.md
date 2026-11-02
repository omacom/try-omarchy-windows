# Normal-session retirement witnesses

**November 2, 2026:** the normal root service now disables the legacy diagnostic
PID-named retirement file. Native retirement and release remain mandatory.
The follow-up completes **432 native 3840×2160 GPU handoffs** and retires three
actors belonging to one process without the earlier marker collision. **Cleanup
still fails, and no complete Omarchy scene is captured.** Counts remain backend
**4/8**, desktop **0/4**, diagnostic scene **3/3**.

## What changed

The earlier run retired one Quickshell actor, then failed when another actor
from the same process tried to create the same exclusive diagnostic file.
The normal root service now supplies `WDDM_BRIDGE_RETIREMENT_WITNESSES=0` after
clearing its environment. The broker accepts this only with its root-supervised
session mode; peer clients cannot select the policy. Coordinator retirement and
actor release still happen before a witness could be written. The diagnostic
default and its exclusive root-owned files are preserved.

The strict production broker and three CPU controls compile. Real Unix/pidfd
session controls exercise rolling connections and owner exit. Filesystem controls
exercise 20,000 disabled same-PID witness calls without creating files or leaking
descriptors, and preserve legacy collisions, ownership and no-follow rejection.
The 34 root-service controls pass with the explicit normal policy checked. Two
generator controls preserve the finite diagnostic default. GPU/native devices
are mocked in these CPU controls; they are not desktop readiness evidence.

## Actual GPU follow-up and retained failures

The image changes only the production broker and normal root service from the
previous image. Independent archive checks verify all 403 files, 68 directories,
23 symlinks, ownership, modes, hashes and gzip integrity. The private NVIDIA
runtime is not redistributed. Host/QEMU binaries and official Windows input code
are unchanged from the previous frame-transport experiment.

Run 8 records frames 1–432 exactly, with matching producer and consumer fences,
root-adapter verification, an active borrowed native boot console and GPU BGRA
conversion. No CPU texture upload or ordinary CPU fence wait is reported. These
are transport observations, not displayed-FPS, complete scene or performance proof.
Three Quickshell actors for one PID retire; no marker-write collision occurs.

Two broker receive operations still fail with `ECONNRESET`. Other actors defer
retirement while global GPU work is busy. Terminal group draining does not finish
before VM exit, so SDL retirement sees disconnected QMP. The native owner exits
3 and the owned VM exits 90. **Successful GPU cleanup is not established.** The
owned job contains and reaps all descendants; the source disk and installed
NVIDIA driver hashes remain unchanged. The inactive desktop never becomes the
input desktop, and neither host nor guest input is injected.

The scene fixture requests absent Alacritty; read-only discovery finds Foot in
this image. Its readiness check therefore fails and produces no accepted capture.
A proposed live correction is refused by the held-process guard after VM exit;
no guest correction command executes. No complete wallpaper/application screenshot
or responsive desktop claim is made.

The [receive-reset follow-up](BROKER-RECEIVE-RESET.md) now passes real-socket
and pidfd CPU controls; GPU integration is pending. Next work is safe terminal
draining of deferred actors and a corrected Foot scene fixture. The
[terminal scheduler implementation](TERMINAL-GROUP-RETIREMENT.md) now passes CPU
controls and both Windows builds; paired GPU verification remains pending. Normal
launcher activation, physical input latency, browsing, multiple windows, sustained
sessions, bounded resources and recovery remain open. CUDA/OpenGL/Vulkan/video,
DLSS, hardware ray tracing and at least 93% matched-native performance still need
their own implementation and acceptance evidence.

[Exact sources and structured evidence](evidence/normal-session-retirement-witnesses-2026-11-02.json) ·
[Earlier 61-frame capture and fixes](NORMAL-SESSION-FIRST-GPU-FRAMES.md) ·
[Daily-use acceptance](DAILY-USE-READINESS.md).
