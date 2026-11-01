# Native launcher lifetime control

**Host-session follow-up, November 1:** an opt-in launcher-owned host transport
passes real idle/backpressure controls and the complete bounded 4K application
regression. Guest lifetime limits, normal startup, physical input and daily-use
acceptance remain open. [Exact scope and remaining integration](LAUNCHER-HOST-SESSION.md).

The native bridge now accepts a private inherited cancellation handle and
publishes the actual owned QEMU identity. Hidden QEMU/WHPX tests verify normal
GPU completion, cancellation and creator exit. **The Go launcher does not yet
activate this protocol. Normal guest startup, official input, sustained desktop
use and matched-native performance remain unverified.** Counts stay **4/8,
0/4, 3/3**.

## Ownership and protocol

An opting-in controller creates an unnamed manual-reset event and explicitly
inherits it into its newly created bridge process with
`--driver-launcher-cancel-handle <handle>`. The bridge rejects malformed handles,
duplicate options, non-owned VM modes and missing context/query permissions
before startup. It captures and holds its actual live creator process, including
creation time. This is a private parent/child capability, not a network endpoint
or authentication by a claimed PID.

The bridge takes ownership of its inherited entry, duplicates it with
`SYNCHRONIZE` access only and removes inheritance. The controller retains its
separate signalling entry. Closing a controller handle alone does not request
cancellation. Windows documents this shared-object relationship in
[handle inheritance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/handle-inheritance)
and the explicit child
[handle list](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute).

After the existing suspended-create, verified adoption, job assignment, resume
and private QMP setup, stdout includes `nativeLauncherVmReady`, protocol 1 and
`launchMode=backend-owned`. It identifies the actual bridge, controller and QEMU
PIDs. Both creation timestamps are decimal strings, preserving all 64-bit values
when consumed by software that uses floating-point JSON numbers. The native
owner retains the verified process object, image file and private job.
[Windows process handles](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)
remain valid after their process exits, allowing exit verification against the
held object.

This message explicitly reports `guestGraphicsVerified=false`,
`launcherInputVerified=false` and `dailyUseAccepted=false`. QMP/VM identity
readiness does not establish graphics or desktop readiness.

A joined watcher waits for either the held creator to exit or the event to be
signalled. It stops and reaps only the held QEMU process. The existing serving
thread retains responsibility for native GPU objects and aperture teardown.
`nativeLauncherStopAcknowledged` reports guest exit with
`driverCleanupComplete=false`; it must never substitute for the separate final
driver-cleanup evidence. While awaiting the first guest connection, the native
thread checks cancellation at 250 ms intervals. The original single 60-second
wait remains unchanged when this option is absent.

## Verified limits, October 29, 2026

Both strict default and 4K Windows builds pass **37 lifetime checks**, the
existing **48 process-ownership checks**, and **13 lifetime CLI controls**.
Helpers verify wait-only/non-inherited retention, creator signal authority,
duplicate starts, cancellation and creator waits, joining without stopping a
live child, callback errors, exact identity timestamps and rejection after
cancellation. Portable consumer controls reject altered identities, missing
fields, boolean/integer aliases and lossy numeric timestamps.

Three new hidden, disk-free guests use the same private images, NVIDIA 621.01
runtime snapshot, byte guards and original GPU/data oracles as the preceding
ownership checkpoint:

| Workload with lifetime protocol enabled | Result |
| --- | --- |
| Vulkan buffer pressure | Original refusal/free/same-buffer GPU retry and both complete 16,384-word rounds pass; 13 submissions and 20 signals retire. |
| Original 4K compute | Both rounds verify all 8,294,400 words. |
| Original 4K shader | Both 3840×2160 frames pass complete pixel checks and shared-texture handoff; pixels match prior captures byte for byte. |

The Python integration consumer independently opens and holds the live QEMU
object, verifies creator, image and creation time, and keeps that handle until
exit. Its recorded actual native command includes the inherited capability;
the historical harness command remains separately recorded. All three guests
and native owners exit naturally with code zero. Native GPU objects and mappings
retire before VM exit. The exact historical GPU harness is unchanged. Generic
initialization acceptance remains false for the Vulkan workloads; their existing
full GPU validators establish the scoped results.

Two separate three-process controls stop after VM identity, before any GPU
mapping is created:

| Trigger | Observed time to controller/VM/native completion | Held process exits |
| --- | ---: | --- |
| Actual creator signals cancellation | 0.135 s | QEMU forced 90; native 1 |
| Actual creator exits while native owner remains live | 0.126 s | QEMU forced 90; native 1 |

Both preserve the correct stop reason and zero final mappings. These are single
early-lifecycle observations, not latency guarantees, repeated crash stress or
recovery during GPU work. Cancellation during native construction, GPU load,
concurrent pressure and broad application failures still require coverage.

The first CLI run found a validation-order mismatch in the 4K profile; the
lifetime mode checks now run before the profile guard. A separate early stop
helper omitted the mandatory private capture directory and was rejected before
QEMU creation. Both failures and their original logs remain retained and
excluded from success evidence.

[Sanitized file-audit evidence](evidence/NATIVE-LAUNCHER-LIFETIME-2026-10-29.json)
binds the frozen native compiler inputs, binaries, raw receipts, prior GPU
controls and the actual command boundary. All **430 Go/module launcher sources
remain unchanged**, including keyboard, cursor, window and QMP input routing.
No visible VM/renderer, host input or desktop control was used.

The subsequent [Go supervisor checkpoint](GO-NATIVE-GPU-SUPERVISION.md) consumes
this identity, publishes the actual QEMU PID and uses cancellation while retaining
the native owner through verified VM exit. Original-launcher argument composition,
matching normal guest startup and end-to-end session validation remain required
before enabling this backend in the product.
