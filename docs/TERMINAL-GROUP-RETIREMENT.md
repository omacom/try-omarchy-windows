# Terminal cleanup across exited GPU actors

**November 2, 2026:** terminal cleanup now drains presentation once, then advances
native retirement across all exited actors in bounded round-robin steps. CPU
controls and both complete strict Windows builds pass. **The updated host and
guest have not yet been tested together on the GPU.** Run 8 remains failed cleanup
evidence; daily-use acceptance stays open.

[November 3 GPU follow-up: group cleanup is reached, but QEMU retirement and Quickshell still fail](NORMAL-SESSION-GPU-ATTEMPT-2026-11-03.md).

## Failure and implementation

The previous terminal handler required every secondary actor to be reclaimed
before stopping presentation. A secondary actor deferred on global GPU work
therefore prevented the presentation drain it could need. A focused CPU
reproduction also demonstrates why finishing one actor at a time is insufficient:
an app's adapter may remain busy until another actor releases its device.

The terminal handler first verifies that every retained actor is disconnected
and holds no shared references. The trusted guest broker supplies those states
after process-exit and capability checks. It seals the group against new work,
stops presentation once, and visits actors in turn. Each request attempts at most
16 native object retirements. Busy results retain ownership and allow another
actor to advance. An actor is removed only after its objects are empty and the
router confirms detachment. GPU fence and native-result checks remain unchanged.

The response reports the selected actor's progress and the number of actors still
owned by the group separately. An empty app cannot signal group completion.
Coordinator protocol 5 requires a paired host/guest update; older protocol or
response sizes are rejected. The guest retains its eight-second wall budget and
a finite step budget covering eight actors, up to 1,024 objects per actor and
four retirement phases. Busy retries consume that budget; exceeding it fails
cleanup rather than releasing live resources.

## Verification and remaining work

The native coordinator controls cover eight actors, an initially empty app,
cross-actor dependencies, GPU and native busy results, live-actor/shared-reference
rejection, failed presentation drain, sealed admission, invalid progress fields
and repeated completed-group requests. Existing guest capability/FD and root
session controls pass. The production broker builds with its existing feature
flags. Both complete Windows host builds pass with strict warnings. These are
CPU and build controls with mocked native GPU resources, not hardware recovery
or performance evidence.

Next is a fresh paired runtime/image and an inactive-desktop GPU test, using the
installed Foot terminal in the scene fixture. The test must demonstrate actual
presentation drain and native resource cleanup before VM exit, then a complete
wallpaper/application scene. Normal launcher activation, responsive browsing and
multiple windows through 4K, sustained sessions, bounded resources, recovery,
broad API coverage and at least 93% matched-native performance remain open.
Counts stay **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

[Exact sources and test receipts](evidence/terminal-group-retirement-2026-11-02.json) ·
[Receive-reset fix](BROKER-RECEIVE-RESET.md) ·
[Run 8 observations](NORMAL-SESSION-RETIREMENT-WITNESSES.md).
