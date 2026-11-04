# Retirement polling and graphics deadlines

**November 4, 2026:** source changes remove a live graphics-thread join from the
coordinator RPC path and give SDL command/state exchanges one bounded deadline.
CPU controls pass and both Windows production-owner configurations compile.
The production owners are **never executed** in these checks. Hardware testing
on this computer remains [on hold](HOST-DISPLAY-INCIDENT-2026-11-03.md).

## Shutdown behavior

Previously, `RetireStoppedGroup` called `stopCoordinatedPresentation`, which
joined the presentation thread inside the controller request. GPU waits, QMP
and VGA restoration could hold that request without returning progress.

The native owner now requests stop and queries the held thread handle with a
zero-time wait. While the thread is alive it reports pending; the coordinator
returns the existing `EBUSY` response with no progress payload. The group stays
sealed and its actor/object counts remain charged. No native actor destruction
starts on the pending path. The owner joins and performs final checks only
after Windows reports the actual thread terminated, including thread teardown.
Failure remains fatal to the coordinator; it never becomes a cleanup receipt.

The guest retries only `EBUSY` from a validated coordinator reply, counting each request toward
the existing attempt limit. A distinct internal pending exception separates that
reply from an operating-system `EBUSY` while opening/transferring the native port.
Transport failures and malformed replies propagate.
Its eight-second admission window and 8,224-attempt cap remain fixed, and a
completion observed after the admission deadline is rejected. Individual wire
transfers still have their separate ten-second failure bound. This is not a
hard eight-second cancellation guarantee for a blocked system call.

The host and guest changes must ship together. The wire format, protocol version
and existing busy status remain unchanged; an older guest that treats every busy
reply as fatal can abort during the new pending phase. Frozen private images are
not automatically upgraded or validated by these source checks.

## Graphics transaction budget

QEMU's teardown fence can wait five seconds, equal to the previous host QMP
command budget. That left no allowance for teardown and a reply when the fence
used its full permitted wait. The graphics connection now has a seven-second
budget, below the guest admission and wire-transfer bounds. Ordinary control
and input connections retain their five-second budgets.

The relay's `qom-set` acknowledgement and `qom-get` state query consume that
**same absolute deadline**. The state query cannot restart the budget. The
acknowledgement, correlated replies, state checks, actual GPU fence wait and
fail-closed ownership checks remain required. The source contract test checks
the QEMU fence duration against the host, guest and transport definitions.

## CPU verification and limits

- A real connected Windows loopback peer replies after 5.2 seconds. Ordinary
  control times out; the graphics budget accepts it. No renderer or VM is used.
- The production transaction helper rejects a late second reply and malformed
  acknowledgement/state replies, using a fake clock.
- A held kernel thread remains pending through 1,000 zero-time queries. It is
  joined only after its exit handle signals. That worker only waits on an event.
- The production guest polling loop rejects endless busy replies, an exhausted
  attempt cap, transport failure, malformed progress and late completion.
- Native mock coordinator controls preserve actors/objects through three
  pending polls, keep the group sealed and reject failed presentation drain.
- Strict `/W4 /WX /O2` compilation passes for the default and 4K Windows owners;
  neither owner executable runs. The unrelated unfinished kernel work is excluded.

The first sandboxed CPU attempt fails at loopback connection. The same frozen
sources and command file pass outside that sandbox restriction; both attempts
are retained. There is no GPU, VM, WSL, host input, display setting or driver
operation in either attempt.

These controls establish software ordering and timeout behavior. Windows driver
calls, native GPU quiescence, the actual VGA return and recovery still require
hardware evidence on an authorized test system. The Quickshell uniform-copy
crash and host display incident remain unresolved. Counts stay **backend 4/8,
desktop 0/4, diagnostic scene 3/3**; full daily use and the original API and
performance requirements remain open.

[Exact CPU evidence](evidence/retirement-polling-cpu-2026-11-04.json) ·
[Run 10 observations](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md) ·
[Daily-use gates](DAILY-USE-READINESS.md).
