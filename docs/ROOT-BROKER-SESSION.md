# Root-supervised guest broker sessions

**November 1, 2026:** a matched host/broker configuration passes all **1,800 GPU
handoffs**, complete first/final **3840×2160 source/SDL pixels with zero maximum
channel error**, both Chromium GPU lifetimes, Nautilus and live cleanup. The root
supervisor explicitly selects the broker's new lifetime policy. The guest's root
service, account and compositor workload remain diagnostic and bounded.

The backend is still an engineering prototype. Counts stay **backend 4/8,
desktop product 0/4, diagnostic scene 3/3**. This component does not establish a
normal persistent Omarchy desktop or daily-use readiness.

## Broker lifetime and admission

The old per-application Unix transport already permits idle connections while
watching each authenticated process. Its credential, pidfd, packet and transaction
checks are retained. The new root-only `WDDM_BRIDGE_BROKER_SESSION=1` selection
removes the broker's global diagnostic alarm and 64 total-connection cap. Other
present values fail. Legacy mode retains both limits.

Before enabling the new mode, the broker requires coordinated live retirement
and pins the **actual root supervisor** through `SO_PEERPIDFD` on its existing
trusted root control socket. Root credentials, actual parent identity, pidfd
identity/liveness and close-on-exec are checked. A client cannot supply a numeric
supervisor PID or take over the primary role. The socket option is defined in
Linux's [UAPI](https://github.com/torvalds/linux/blob/v6.11/include/uapi/asm-generic/socket.h).

Connections can roll over time while the negotiated **four or eight simultaneous
workers** remain bounded. Completed threads are joined and removed before slots
are reused. Primary registration, actor/resource/byte agreements, GPU APIs and
Windows/SDL/QMP input code are unchanged. The counter saturates diagnostically
rather than turning a finite integer into another lifetime admission cap.

The primary must connect within **30 seconds** after its trusted identity is
received. After the primary ends, secondary peers have a **ten-second drain
budget**. Unexpected supervisor loss, drain expiry or controller failure in this
mode exits the broker with status **125**, without joining peers that might remain
idle. A best-effort nonblocking fault record preserves inherited diagnostic file
flags. Closing the controller relies on the existing Windows owner to reap QEMU
before releasing retained GPU mappings. This is a containment design; actual
supervisor-loss containment during GPU rendering remains unverified.

## Real IPC controls and compilation

The actual shared admission loop runs against real Linux Unix sockets,
credentials and process descriptors, with **GPU work explicitly mocked**:

| CPU/IPC control | Observation |
| --- | --- |
| Legacy connection cap | 64 accepted, 65 subsequent refusals |
| Rolling session connections, four-worker profile | 145 accepted, peak 4 |
| Rolling session connections, eight-worker profile | 145 accepted, peak 8 |
| Additional simultaneous peers while all slots held | Wait until a slot retires |
| Same-UID process attempting the primary role | Rejected |
| Actual root-supervisor death with idle primary still alive | Broker exits 125 |
| Inherited diagnostic file-status flags | Unchanged |

Separate virtual-clock controls cover startup/drain boundaries and healthy policy
state at a later time. They do not represent a day-long guest run. Every owned
test process is reaped; these controls use no VM, GPU, desktop or physical input.

Four current public broker configurations compile with strict C++17 warnings.
The actual legacy broker ELF is first reproduced **byte for byte**, then rebuilt
with the new policy. Its existing pending-at-capacity diagnostic is retained;
an exact reverse transformation recovers the public session header. The same
real IPC controls also pass against that legacy closure.

The guest generator accepts only an explicit boolean for broker selection. Its
default generated supervisor remains byte-identical. The selected version adds
only the root environment selection and a diagnostic marker. The original root
supervisor is reproduced byte for byte before the legacy build; separate current
public-header C11 compilation and diagnostic controls pass.

## Bounded GPU run 32

| Hardware observation | Result |
| --- | ---: |
| Handoffs, authenticated END and ACK | 1,800 |
| Workload elapsed / unchanged deadline | 85.064954 / 90 seconds |
| Complete first/final source and SDL pixels | 3840×2160, maximum error 0 |
| Native submissions / failures | 4,341 / 0 |
| Device-health queries / failures | 4,807 / 0 |
| Peak allocation records / unchanged limit | 1,079 / 2,048 |
| Peak reported GPU usage / unchanged pool | 1,360,363,520 / 4,294,967,296 bytes |
| Real broker connections / peak workers / limit | 12 / 6 / 8 |
| Chromium GPU actor 9 / actor 12 accepted native commands | 83 / 65 |
| Nautilus actor 10 accepted native commands | 9 |
| Three mapped applications at four checkpoints | Pass |
| Browser interactions / GPU resizes / terminal resizes | 2 / 4 / 3 |
| Guest-only original QMP input commands | 33 |
| Natural exit and tracked native cleanup before exit | Pass |

Both browser sessions retain HTTPS, NVIDIA shader, compositing, observed-thread
sandbox and quiescent native retirement checks. Per-actor queue acceptance remains
separate from the GPU completion and retirement evidence. Both selected lifetime
modes and the broker's joined-worker summary are required by the audit.

The first core audit rejects the root selection record because an existing tty
capability response directly precedes it. Raw logs remain unchanged. A fresh
auditor accepts only the complete clean record or that **exact observed prefix**;
altered prefixes or suffixes are rejected. An intermediate escape-decoding error
is retained separately. All original pixel, GPU, application and cleanup guards
remain required.

Timing is **not stable yet**: run 31 took 61.278317 seconds, while this run takes
85.064954 seconds, close to the same 90-second deadline. The cause is not isolated;
this is not evidence of a performance improvement, displayed FPS or the requested
93% matched-native result. It remains an open performance investigation.

Only the broker and root-supervisor payloads change from the prior guest; every
other payload, archive metadata and tail are compared. This yields eight cumulative
payload replacements from guest 204. Earlier diagnostic provenance retains its
historical scope, with current broker/supervisor proof recorded separately.
Source disk and installed NVIDIA hashes remain unchanged. All VM surfaces stay
on an owned inactive Windows desktop, with no desktop switch or host input.

## Remaining product work

The root service still has its diagnostic lifetime and the compositor still runs
a finite workload. Ordinary provisioning, SDDM → Omarchy desktop → UWSM startup,
a persistent compositor/output stream, physical Windows input responsiveness,
hours of browsing/multiple applications, pressure recovery and active-rendering
interruption tests remain required. General Vulkan windows, CUDA, video
acceleration, DLSS, hardware ray tracing and at least **93% matched-native
performance** remain unverified. No daily-use notification follows from this run.

[Structured results, source hashes, timing limits and retained failures](evidence/root-broker-session-run32-2026-11-01.json).
[Earlier actual desktop and shader captures, retaining their run-30 scope](ORIGINAL-PROFILE-APPLICATIONS.md).
