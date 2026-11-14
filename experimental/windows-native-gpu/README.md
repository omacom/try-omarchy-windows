# Native Windows GPU experiment

The opt-in [Vulkan GPU-buffer pressure recovery](../../docs/VULKAN-GPU-PRESSURE-RECOVERY.md)
now passes two fresh refusal/free/retry/data/cleanup runs under unchanged byte
guards. It is advisory external-buffer admission; general desktop pressure,
concurrency and daily-use acceptance remain open.

## Application allocation metadata

The default-off `--driver-application-objects` option and matching
`WDDM_BRIDGE_APPLICATION_OBJECTS=1` guest-controller build negotiate 2,048
aggregate allocation records through object contract version 10. Both sides
must opt in; older requests retain 1,024 records. Per-actor and byte limits stay
fixed. [Agreement, tests and retained three-app failures](../../docs/APPLICATION-OBJECT-CAPACITY.md).

## Application memory agreement

The default-off `--driver-application-memory` option and matching
`WDDM_BRIDGE_APPLICATION_MEMORY=1` guest build negotiate a separate 112-byte
owner/controller initialization and a shared 4 GiB reported-usage pool. Older
payloads retain their limits. Native headroom admission, exact authenticated
capacity matching and retained default/enabled desktop evidence are documented
in [Application memory](../../docs/APPLICATION-MEMORY.md). This does not establish
browser, pressure-recovery or daily-use acceptance.

**Product integration priority:** preserve the official launcher's keyboard,
cursor and reserved-key logic and QEMU SDL input path. Connect the graphics
backend to that frontend. The fixed QMP test and separate native window remain
diagnostics. [Verified upstream source map and integration boundary](../../docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).

The next graphics boundary now has two passing **host SDL/D3D12 interop controls**:
named 4K GPU textures, distinct producer/consumer fences and independently
verified complete source/SDL backbuffers. The window stays hidden; QEMU guest
scanout remains unverified. A separate OpenGL-enabled QEMU build candidate
retains all 24 official runtime patches and passes the existing input controls.
See [frozen evidence, build instructions and scope](../../docs/LIVE-SDL-D3D12-INTEROP.md).

Two fresh guests now pass **300 actual 4K NVIDIA handoffs with real application
resizing**. The same terminal grows to 1440×960 and 1600×1000, then returns to
1200×800 while handoffs continue. Complete first/last GPU buffers independently
verify the enlarged panel and restored wallpaper; input response and live cleanup
also pass. Two earlier 300-frame input-response baselines remain separate.
See [actual images, four-run evidence and scope](../../docs/LIVE-UWSM-SUSTAINED-RESIZE.md).
Hardware staging reallocation during rendering, physical Windows input and
normal launcher/continuous-display acceptance remain open.

Two fresh 4K NVIDIA guests pass the complete Alacritty PTY input-response audit
after safe reclamation of idle staging textures under residency pressure. Each
actually releases 31.875 MiB after both copy fences retire, preserves three-slot
capacity and passes live cleanup under the unchanged 1 GiB usage guard.
Hardware reallocation during continued rendering, physical Windows input and
continuous desktop presentation remain unverified.
See [actual frames, two-run evidence and limits](../../docs/LIVE-UWSM-STAGING-RECLAIM.md).
The [earlier input-response repeat failure](../../docs/LIVE-UWSM-QMP-INPUT-RESPONSE.md)
retains its original rejected scope and evidence.

The [native input discovery checkpoint](../../docs/LIVE-UWSM-NATIVE-INPUT-DISCOVERY.md)
passes two fresh UWSM guests with the actual virtio keyboard and tablet enumerated
alongside five NVIDIA application cycles, twelve 4K handoffs and complete cleanup.
It reuses the existing logind seat and defers initial input events until the
compositor event loop starts. Typed input, Windows pointer delivery and launcher
integration remain unverified.

The [guarded staging and terminal cleanup checkpoint](../../docs/LIVE-UWSM-DEMAND-STAGING.md)
passes two fresh UWSM/PAM guests, each with five GPU application cycles and twelve
4K handoffs. Staging textures are allocated on demand without reducing ring
capacity or admission limits. A trusted terminal group operation requires every
actor to exit and drain its capabilities, seals admission, drains native presentation
and retires the primary's remaining objects before disconnect. Earlier failures
remain retained. Long-run memory stability and normal launcher/input remain open.

The earlier [UWSM-managed NVIDIA desktop checkpoint](../../docs/LIVE-UWSM-GPU-SESSION.md)
passes one fresh PAM/logind session with independently verified active service
state, five GPU application lifecycles, twelve audited 4K handoffs and no
mappings deferred to VM exit. It adds root-selected pidfd compositor admission;
the controlled factory/session fixture is still separate from normal launcher,
input and sustained desktop acceptance. Its repeat exceeds the unchanged GPU
usage guard and remains rejected. Earlier checkpoints below keep their
original scope and limitations.

The [repeated compositor presentation checkpoint](../../docs/LIVE-HYPRLAND-QUEUED-PRESENTATION.md)
passes twelve actual Hyprland submissions per fresh guest, twice. The new
`WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT=1` startup mode uses two audit readbacks and
ten GPU-only ordinary frames. It needs the explicit native presentation opt-in;
it cannot be combined with the earlier capture mode. The native UI thread,
three staging slots and imported source cache are bounded and explicitly released.
The profile-4 command/request budgets remain finite. Display reliability,
normal launcher integration and the full Omarchy desktop remain unverified.

The [paced compositor handoff](../../docs/LIVE-HYPRLAND-FRAME-PACING.md) separates
native submission from display telemetry. Two fresh guests pass exact deferred
display observations for both frames; an earlier targeted ETW trace confirms
display despite stale DXGI statistics. The compositor path has no per-frame
statistics polling or fixed three-second hold. CPU readback remains diagnostic;
sustained rendering and normal launcher integration remain unfinished.

The [actual Hyprland native-window experiment](../../docs/LIVE-HYPRLAND-NATIVE-WINDOW.md)
passes two strict display checks once; its immediate repeat fails at the second
DXGI statistics check despite correct backbuffer bytes. The retained presenter
reuses the NVIDIA device, queue and window across compositor buffers. A fresh
capture-only regression passes. Stable display and normal launcher integration
remain unverified; no desktop gate is closed.

The [actual compositor capture checkpoint](../../docs/LIVE-HYPRLAND-COMPOSITOR-CAPTURE.md)
copies Hyprland's current owned output buffer on the Windows GPU after a native
renderer-fence wait. Four captures in two fresh guests pass known-background
reference and lifecycle audits. No guest CPU map or CPU upload is used.
Full scene correctness and Windows app presentation remain unverified; this
does not close a desktop gate. The public startup probe enables the bounded
two-capture mode only with `WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE=1` at build time.

The [actual Hyprland shutdown fix](../../docs/LIVE-HYPRLAND-LOGGING-LIFETIME.md)
removes a global logger's strong backend ownership. Three fresh bounded
compositor runs, including two without fault tracing, now exit cleanly with all
GPU mappings and broker views explicitly released. Seven output-buffer commits
per run were pixel-unverified at that earlier checkpoint. Apply the additive Aquamarine `logging-lifetime` patch
after the native-render patch. Desktop progress remains 4/8 stages and 0/4 gates.

The [process-exit lifetime fix](../../docs/LIVE-GPU-EXIT-LIFETIME.md) now keeps
native CPU views valid through late driver-library finalizers. The old bridge
segfaults in a deterministic real-fence control; three fixed guests pass, along
with two fresh Mesa timeline FD regressions. Long-run stability, the separate
legacy V1 capacity rejection and full desktop integration remain open.

The optional [guest kernel aperture component](guest-kernel/README.md) now
manages restricted native-memory leases, with two real OpenGL runs and
cross-process FD revocation [verified in QEMU](../../docs/LIVE-GPU-KERNEL-APERTURE.md).
It is not DRM, dma-buf or a kernel implementation of graphics ioctls. Its kernel
source is GPL-2.0-only; the userspace ABI and contract probe are MIT.

The separate [4K lab profile](../../docs/LIVE-GPU-4K.md) now renders and verifies
3840×2160 shader frames and native backbuffer copies. One presentation run passes
both statistics receipts; subsequent stale-counter failures remain preserved.
It adds bounded larger-resource builds, compact binary captures and exact
presentation timing records. Display stability and sustained performance remain
unfinished.

Experimental Windows NVIDIA driver bridge for the 7Wdev fork. The genuine Linux
NVIDIA runtime in an owned QEMU/WHPX guest now passes
[full D3D12 device and copy queue initialization](../../docs/LIVE-D3D12-INITIALIZATION.md)
and [GPU buffer copies with verified guest readback](../../docs/LIVE-GPU-BUFFER-COPY.md).
Two normal RTX 5090 Laptop runs each copy two distinct 64 KiB patterns through
UPLOAD, DEFAULT and READBACK resources; every byte matches. Native command and
queue-signal fences retire, with normal probe/VM exit and verified cleanup.
This does not establish accelerated Omarchy, the requested APIs or the minimum
93% native performance.

The [shader diagnostic](../../docs/LIVE-GPU-SHADER-TRIANGLE.md) passes two fresh
physical runs with every pixel checked in two distinct rounds. A separate
[shared-texture route](../../docs/LIVE-GPU-SHARED-RESOURCE.md) also passes two
fresh runs: the guest draws into a shared texture and Windows independently
imports and releases that resource before drawing. Select `--runtime-workload shared`
in both the private-image packer and owned-runtime verifier, plus the separate
`--driver-shared-resources` native opt-in. A subsequent
[retained Windows GPU consumer](../../docs/LIVE-GPU-SHARED-TEXTURE-CONSUMPTION.md)
passes two fresh runs with two distinct frames each. It requires an explicit
COMMON/fence handoff, `--runtime-workload consume` and `--driver-consume-shared`;
every native readback byte matches the guest shader output. The subsequent
[native DXGI presentation checkpoint](../../docs/LIVE-GPU-PRESENTATION.md) adds
`--driver-present-shared`: actual backbuffer bytes and completed frame statistics
pass for both guest frames in two fresh runs. An intermittent native import
failure was reproduced in a separate control and remains unresolved. Import
stability and Omarchy desktop presentation remain unfinished.

The newer [typed texture metadata experiment](../../docs/LIVE-GPU-NATIVE-METADATA.md)
adds capability bit 30 and `CreateNativeSharedVendorResourceAllocation` (`0x205b`).
Select `--runtime-workload native` when packing/verifying the private image and
enable `--driver-native-metadata` alongside the shared-resource/consumption gates.
A one-use declaration binds the diagnostic texture profile to an owned device
and calling thread. Windows generates its own sharing metadata through a native
template; guest metadata stays unchanged. Three fresh two-frame consumption runs
pass. Both newer presentation attempts fail DXGI statistics despite successful
imports and matching first-frame backbuffers; the evidence preserves their
rejections. Opaque metadata reuse is experimental and not a documented ABI.

The diagnostic adds owned, nonshared single-allocation resources, the observed
NVIDIA DEFAULT-resource DriverProtection value, direct CPU fence waits and a
bounded GPU2 context-signal route. `--runtime-workload copy` must be selected
both when packing the private guest image and when verifying it. The default
remains initialization only. The copy guard rejects incomplete, duplicated or
incorrect readback results and requires matching native resource/signal evidence.

The [graphics clear workload](../../docs/LIVE-GPU-CLEAR.md) now passes two
physical runs: a DIRECT queue renders two distinct RGBA8 images, with every
pixel verified after GPU readback. Separate asynchronous command acceptance and
hardware-queue signaling preserve ownership until actual native retirement.
The earlier [graphics queue controls](../../docs/LIVE-GRAPHICS-QUEUE-CONTROL.md)
remain lifecycle tests. Initialization and copy controls cannot satisfy the
clear workload verifier. The separate shader and shared-resource guards require
their own drawing/import evidence; desktop presentation remains unfinished.

The optional `--hwqueue-no-broadcast-wait-eof-test` stops the clear workload
after its first combined `NoBroadcastSignal | NoBroadcastWait` queue (flags 6).
It requires submission, retirement and a separate run from other EOF controls.
Its verifier requires unchanged native flags, acknowledged earlier unmaps and
verified native cleanup after owned VM exit. It cannot certify rendered pixels.

The bridge preserves source sentinels, TDR fence flags, native allocation
subranges and verified in-process context priority. Those baseline GPU mappings have a separate
32 MiB cap; CPU views retain their 16 MiB live-byte cap and 4 MiB per-view limit.
The [CPU aperture](../../docs/LIVE-CPU-APERTURE-CAPACITY.md) defaults to 16 slots
and supports 32/64 slots. The optional 128-slot graphics startup profile
negotiates a 32 MiB live vendor CPU-view budget and a separate 64 MiB GPU-map
budget and a 128-allocation live-object limit; other profiles retain 16 MiB CPU,
32 MiB GPU and 96 allocation objects. Per-request allocation lists remain bounded
to 96 entries.
It also selects 128 read-only fence pages; other profiles retain 64.
This profile passes the bounded clear diagnostic. [Contiguous views](../../docs/LIVE-CPU-ALLOCATION-SPANS.md)
span adjacent slots without a QEMU rebuild. Native pools allow 96 independent
synchronization objects and 256 aggregate wire objects; the
default read-only fence BAR has 64 pages. Earlier hardware acceptance includes repeat, owned
VM exit, old-guest, capacity-rejection and restored CPU write controls.

The explicit [256-slot virtual layout](../../docs/CPU-APERTURE-CAPACITY.md)
keeps the corresponding 128-slot profile's physical/object quotas. The 4K
profile's CPU limit remains 96 MiB with a 64 MiB per-view bound; its GPU mapping
limit remains 256 MiB. Two actual guests now verify a full 33 MB resource-FD
readback using that layout. Older profiles retain their existing limits.
This bounded comparison does not establish general fragmentation handling or
desktop stability.

[Allocation retirement](../../docs/LIVE-ALLOCATION-RETIREMENT.md),
[bounded command submission](../../docs/LIVE-COMMAND-SUBMISSION.md),
[GPU reservations](../../docs/LIVE-GPU-RESERVATION.md) and
[Zero address state](../../docs/LIVE-GPU-ADDRESS-STATE.md) preserve ownership and
verify native progress. General submission, production asynchronous transport and signal/wait,
eviction, guest kernel/DRM support and presentation remain unfinished. The
launcher does not select the experiment. Read the
[investigation](../../docs/WINDOWS-NVIDIA-BACKEND.md) and
[product acceptance gates](../../docs/PRODUCT-ACCEPTANCE.md).

Requirements: x64 Windows 10/11, NVIDIA GPU with D3D12, Visual Studio C++ Build
Tools and Windows SDK. The QMP controller uses the pinned MIT nlohmann/json
single header bundled under `third_party`; no driver installation is required.

```powershell
experimental/windows-native-gpu/build.ps1
experimental/windows-native-gpu/build/native-gpu.exe --probe
# Opens a short-lived window, submits 90 frames and verifies shared pixels.
experimental/windows-native-gpu/build/native-gpu.exe --self-test --report native-gpu-report.json
```

The build runs packet contract tests. GPU tests require an interactive Windows
desktop and real NVIDIA hardware; hosted CI only builds the experiment.
The separate [continuous native shader reference](../../docs/NATIVE-GPU-STREAM.md)
adds bounded frame reuse, GPU timestamps and two independent 4K audit captures
as a control for sustained guest rendering and display research.
`--probe` reports adapter/device availability without claiming rendering.
Failures have nonzero exit status; software fallback is refused. Reports
explicitly record that guest acceleration and a custom virtio GPU are absent.
The host rendering test does not boot a VM. The separate disk-free QEMU control
test and its build instructions are in [WDDM bridge](../../docs/WDDM-BRIDGE.md).
The [Linux UMD context experiment](../../docs/UMD-CONTEXT-COMPATIBILITY.md)
records the context acceptance and hardware queue dependency investigation.
The [adapter query bridge](../../docs/ADAPTER-QUERY-BRIDGE.md) adds an independent
`--driver-queries` opt-in, bounded large-buffer transactions, and a reusable
Linux query client. Physical QEMU acceptance includes the NVIDIA runtime's
50,616-byte initialization query. The [live runtime diagnostic](../../docs/LIVE-NVIDIA-RUNTIME.md)
adds explicit local UMD selection and documented PV adapter flag adaptation,
without modifying vendor-private replies. Its
[owned-QEMU paging mode](../../docs/LIVE-PAGING-BRIDGE.md) maps native fences
through a host-only QMP channel and retains pages until unmap acknowledgement
or confirmed owned VM exit. It is a partial initialization
experiment, not a graphics kernel driver or a usable accelerated backend.
The [live allocation route](../../docs/LIVE-ALLOCATION-BRIDGE.md) adds the
separate `--driver-allocations` opt-in and typed standalone allocation ownership.
Its reported-memory guard is not a hard allocation-byte quota; it remains
restricted to the local runtime diagnostic.
The [live GPU-address route](../../docs/LIVE-GPUVA-BRIDGE.md) adds
`--driver-gpuva`, typed queue/allocation ownership, preserved positive NTSTATUS
and direct guest verification of paging completion. The page limit bounds
mapped GPU VA rather than opaque vendor allocation bytes.
The [live residency route](../../docs/LIVE-RESIDENCY-BRIDGE.md) adds
`--driver-residency`, owned allocation lists, preserved paging/count/trim
outputs and a conservative bound on residency attempts.
The [native Lock2 investigation](../../docs/NATIVE-LOCK2-INVESTIGATION.md)
adds a standalone CPU-sharing probe and a separate writable QEMU allocation
hub. The [live CPU allocation route](../../docs/LIVE-CPU-ALLOCATION-BRIDGE.md)
adds `--driver-cpu` only to an owned QEMU runtime. Physical tests passed direct
reads, first/last-word stores restored by Windows, repeated guest references,
capability-disabled behavior and native cleanup after VM exit with a lock held.
The [live hardware queue route](../../docs/LIVE-HARDWARE-QUEUE-BRIDGE.md) adds
documented allocation-token translation and the `--driver-hwqueues` opt-in.
The live UMD builds its private queue data from a translated allocation alias;
public operations still resolve to typed owned wire objects. Direct queue
fences, queue-before-allocation cleanup and guest exit with a queue held passed
physical testing. Exact committed CPU subregions of 4, 16, 64 and 128 KiB work,
including subregions inside larger reservations. Live runtime writes reached
eight views. The [live synchronization route](../../docs/LIVE-SYNCHRONIZATION-BRIDGE.md)
adds `--driver-syncs`, typed monitored-fence/mutex ownership, direct fence reads,
acknowledged destruction and native cleanup after abrupt guest exit.
The separate `--driver-submit` route validates ownership, residency and a
retained CPU command mapping before native submission. It synchronously waits
for retirement as a diagnostic. The optional `--driver-async-submit` selects a
separate negotiated queue operation, returning after KMT acceptance and recording
actual hardware retirement independently. The verifier requires every accepted
command to retire and complete native cleanup. This removes a demonstrated
startup dependency stall and supports the verified graphics diagnostics. Production queues
must also preserve asynchronous
execution and overlap. General allocation layouts, broader synchronization operations,
scanout and desktop integration remain unfinished.
The separate `--driver-retirement` option uses documented VidMm destruction,
keeps `AssumeNotInUse` zero, requires acknowledged CPU unmap and preserves
ownership on native failure. CPU-slot exhaustion returns allocation failure
before taking a native lock or issuing a QMP mapping.
The [product acceptance gates](../../docs/PRODUCT-ACCEPTANCE.md) require CUDA,
OpenGL, Vulkan, video acceleration, a stable interactive Omarchy desktop and at
least 93% of native performance. No current test proves those gates.

The framing reference is pinned in `sources.json`. No upstream implementation
is vendored from virtio-nvgpu; these new files use the repository's MIT license.
The separately vendored JSON parser retains its own MIT license and exact
source hashes, and native builds package that license with the executable. Future copied
upstream code must preserve its per-component license: protocol, device backend
and guest kernel module have different terms.
