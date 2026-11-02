# Broker receive resets retain process lifetime

**November 2, 2026:** a real Linux socket control reproduces the receive reset
seen in normal GPU run 8. The broker now handles `ECONNRESET` as a disconnected
channel while retaining the pinned process descriptor. This fixes a CPU-proven
broker failure path. **GPU cleanup and desktop readiness remain unverified.**

A client can close a Unix seqpacket socket with an unread reply while its process
is still alive. Previously the receive exception marked the entire worker as
failed, preventing terminal group retirement even after the process exited.
The receive path now returns the existing disconnected state for this one errno.
The server still waits for pidfd-confirmed process exit before releasing CPU
views and draining object references. Native retirement remains mandatory.
Malformed packets, credential changes and other receive errors retain their
existing rejection behavior.

The regression first observes `ECONNRESET` through a raw Linux socket. A separate
connection then exercises the production `Peer` code with the same unread-reply
close. The unchanged broker fails that test; the fixed broker passes. Both
controls keep the child alive until explicitly released, check its pinned pidfd,
verify received memory remains valid, and balance descriptors. Existing protocol,
ancillary-FD, finalizer and thread-group controls pass as well.

The full production broker compiles with strict warnings and the same nine
production feature flags. Existing routing and root-supervised session controls
pass, including rolling Unix connections and supervisor exit. Native GPU devices
are mocked in those controls. No VM, renderer or host input is used for this
change, and the fixed broker has not yet been tested on the GPU.

Run 8 remains failed evidence: secondary actors defer retirement while global GPU
work is busy, and terminal presentation draining does not complete before QMP
disappears. Safe draining of those deferred actors is the next implementation
boundary. The normal scene fixture also needs the installed Foot terminal.
Counts remain **backend 4/8, desktop 0/4, diagnostic scene 3/3**. Sustained daily
use, broad API coverage and at least 93% matched-native performance remain open.

[Exact sources and CPU evidence](evidence/broker-receive-reset-2026-11-02.json) ·
[Run 8 and its retained failures](NORMAL-SESSION-RETIREMENT-WITNESSES.md).
