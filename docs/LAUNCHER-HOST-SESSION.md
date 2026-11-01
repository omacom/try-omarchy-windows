# Launcher-owned host transport

**November 1, 2026:** the native host has an explicit `--driver-launcher-session`
mode that allows an idle connection to remain open while its verified launcher
and owned QEMU remain live. In-progress reads and replies still have absolute
deadlines. Both strict Windows builds and real socket controls pass; run **31**
passes the existing full 4K three-application regression using the new mode.

This is the **host transport component**. The guest is still the bounded probe,
released settings do not select this mode, and normal Omarchy session startup
remains incomplete. Counts remain **backend 4/8, desktop product 0/4, scene 3/3**.

## Lifetime and message bounds

The new flag requires the existing original launcher profile, inherited
cancellation capability, held creator monitor and full owned SDL coordinator.
It cannot enable an unmanaged stdin/listener runtime. Ordinary diagnostics retain
their existing 120/240-second deadlines and blocking transport path.

The opted-in host loop uses its exclusive, peer-verified QEMU socket in
nonblocking mode. Idle readiness waits use `select` with at most a **100 ms poll
timeout**, checking the existing launcher/QEMU lifetime between waits. No keepalive
GPU commands, repaint stimulation or extra worker thread is introduced. OS
scheduling and actual cleanup add latency; 100 ms is not a whole-shutdown guarantee.

Once a packet becomes readable, **one ten-second deadline** covers its complete
length header and payload. A permitted size is checked before allocation. Each
reply has a separate ten-second deadline covering all vectored sends, partial
progress and backpressure. Progress does not restart either deadline. These are
transport budgets, not a ten-second whole-GPU-command or recovery guarantee.
Socket success alone does not prove peer delivery or GPU completion, consistent
with Microsoft's [WSASend documentation](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend).

Idle cancellation or owned-QEMU exit ends the loop. Incomplete frames are never
dispatched. Existing SDL import retirement, held-process reaping, actor teardown
and native resource cleanup remain required. Packet layout, GPU APIs, memory
agreements and official Windows/SDL/QMP input implementation are unchanged.

## Verification

The actual Windows loopback controls test separate idle periods, exact packet
and vectored-reply bytes, malformed lengths, truncated headers/payloads, header
and payload trickles under a shared deadline, cancellation with partial data,
and write timeout/cancellation under **observed `WSAEWOULDBLOCK`**. The pressure
fixture observes blocking after 81,920 filler bytes in both controls; it never
assumes that a configured socket-buffer size forces a particular send to block.
A separate control waits **11 seconds idle** with the production **10-second
packet timeout**, then receives the packet successfully using the same header.
These are CPU/socket controls, without a GPU, VM, window or host input.

MSVC `/std:c++17 /W4 /WX /O2` passes in both default and 4K configurations, with the
existing CPU tests, 17 lifetime/session CLI controls and 33 coordinator guards per
profile. Earlier strict conversion and SDK-macro failures remain retained. The
first write-pressure fixture incorrectly assumes a 1 MiB send must block; its
rejection remains recorded and the corrected control requires actual backpressure.

| Bounded hardware run 31 | Result |
| --- | ---: |
| Ordered handoffs and authenticated END/ACK | 1,800 |
| Workload elapsed / unchanged deadline | 61.278317 / 90 seconds |
| Complete first/final GPU source and SDL pixels | 3840×2160, maximum error 0 |
| Native submissions / failures | 4,339 / 0 |
| Successful device-health queries / failures | 4,767 / 0 |
| Peak allocation records / unchanged limit | 1,063 / 2,048 |
| Peak reported GPU usage / unchanged pool | 1,474,105,344 / 4,294,967,296 bytes |
| Chromium GPU actor 9 / actor 12 accepted native commands | 82 / 77 |
| Nautilus actor 10 accepted native commands | 9 |
| Three mapped applications at four checkpoints | Pass |
| Controlled browser interactions / GPU resizes / terminal resizes | 2 / 4 / 3 |
| Original guest-only QMP input commands | 33 |
| Natural exit and tracked native cleanup before exit | Pass |

The audit requires the actually selected host-session marker and rejects the
legacy host diagnostic-budget marker. Both browser lifetimes retain HTTPS,
shader, compositing, observed-thread sandbox, resize and quiescent native
retirement checks. Exact per-actor native queue acceptance remains distinct from
the separate GPU-completion and retirement checks. A copied application's auditor
first rejects a stale run-ID pin; a fresh version corrects that pin without changing
application predicates. The source disk and installed NVIDIA files remain unchanged.

The run stays on an owned inactive Windows desktop, with no desktop switch or
host input. [Earlier actual desktop and shader captures](ORIGINAL-PROFILE-APPLICATIONS.md)
retain their run-30 scope; this change does not replace those captures.

## Remaining integration

The guest broker retains its **240-second alarm and 64 lifetime-connection cap**
by default. A matched root-supervised opt-in now passes scoped IPC and 4K GPU
checks; [its evidence and remaining limits](ROOT-BROKER-SESSION.md) are separate.
The root service and compositor workload are still bounded. The prepared Go profile
does not yet select the new flag automatically. A normal persistent desktop needs
coordinated guest/host lifetime, rolling application connections, official
provisioning and SDDM → Omarchy desktop → UWSM integration. The current probe's
forced account/session must not be treated as that product flow.

This one-minute GPU regression and eleven-second socket idle do not establish
hours of browsing, physical Windows keyboard/wheel latency, interruption during
active rendering, concurrent pressure recovery or matched-native performance.
General Vulkan window support, CUDA, video acceleration, DLSS, hardware ray tracing
and the requested **at least 93% matched-native performance** remain required.
Private binaries, disks, initrds and raw journals are not published.

[Structured hardware results, source hashes and control limits](evidence/launcher-host-session-run31-2026-11-01.json).
