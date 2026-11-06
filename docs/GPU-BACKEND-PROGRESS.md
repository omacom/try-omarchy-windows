# NVIDIA backend progress

Updated **2026-11-06**. This page tracks the experimental QEMU/WHPX Windows-native
NVIDIA backend. The released Try Omarchy launcher continues using the upstream
graphics runtime.

**Upstream and driver update — November 6, 2026:** the development branch is
synced through upstream `5c7ad83`, with all four checks for merge `5e0f33d`
successful. Graphics UMDs matching the user's updated NVIDIA installation are
privately staged. Mesa 26.2.6 Dozen compiles and passes seven recipe and eleven
ICD/file controls remotely. The corrected graphics job compiles all five
libraries and passes twenty actual-fence CPU plus fourteen loader-alias controls,
then stops at the strict `libgallium` ABI comparison. Completed artifact review
confirms only the 83 gallium version tags changed across 527 exports; the exact
transition and dependency lists are now pinned. A new acceptance build is
pending approval; the existing candidate workflow remains failed. No new guest
image or hardware validation is claimed;
counts remain **4/8, 0/4, 3/3**.
[Exact update state and remaining steps](GRAPHICS-DRIVER-UPDATE-2026-11-06.md).

**Guarded guest candidate preparation - November 5, 2026:** a data-only updater
is defined to place the compiled Mesa guards into a fresh normal-root image,
with static ELF dependency checks and exact preservation of other payloads.
Completed controls and a private assembly receipt are required; the recipe
alone adds no desktop acceptance.
[Preparation, bounds and remaining validation](GUEST-MESA-CANDIDATE.md).

**Teardown dependency guard - November 5, 2026:** disconnect and owner
cleanup now retain failed children before attempting parent destruction or later
phases. Native device/adapter guards are added. CPU fault-comparison evidence
requires a completed run for the current revision. No hardware recovery is claimed.
[Fault, source changes and limits](NATIVE-TEARDOWN-PARENT-GUARD.md).

**Vulkan source build recipe - November 5, 2026:** a separate full patched
Dozen build and component archive are defined alongside the desktop build.
Build evidence requires a completed run for the current revision. This does
not implement GPU-backed Wayland presentation or add runtime acceptance.
[Recipe, file controls and limits](MESA-DOZEN-SOURCE-BUILD.md).

![Three GPU apps verified; backend stages 4/8; desktop gates 0/4](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/windows-nvgpu-lab/docs/images/gpu-backend-progress-three-apps-2026-10-27.svg)

## What is verified now?

**Host display incident — November 3, 2026:** the user reports brightness and
graphics corruption across Windows; NVIDIA driver-reset events are recorded.
GPU/VM tests on this host are on hold. Cause and recovery remain unverified;
work continues through source and CPU-only checks. **Daily use is blocked.**
[Incident observations and limits](HOST-DISPLAY-INCIDENT-2026-11-03.md).

**Uploader failure follow-up — November 4, 2026:** CPU fault injection
reproduces lost references and duplicate releases in the pinned Mesa uploader.
The ownership patch passes ten cases. A separate state-tracker guard prevents
null constant uploads in 216 CPU failure cases, with successful copies and
CPU retries preserved. Full runtime, Quickshell and hardware validation remain open.
[Patch, CPU evidence and limits](MESA-UPLOAD-FAILURE-LIFETIME.md).
[Constant-buffer guard and limits](MESA-CONSTBUF-FAILURE-GUARD.md).

**Wayland component staging - November 5, 2026 review:** completed Linux CI
builds the public presentation source and upload guards, regenerates four
protocol files and stages five libraries under `/usr` in an owned destination.
All 527 compared API exports and six loader-alias targets pass; eight source
controls and fourteen filesystem controls pass. The libraries are never loaded.
Runtime and hardware acceptance remain open; counts stay **4/8, 0/4, 3/3**.
[Exact scope and requirements](MESA-OWNED-PRESENTATION-SOURCE.md).

**Full Linux-target source build - November 4, 2026:** the complete selected
D3D12/Wayland/EGL/GBM graph compiles with the upload guards and captured private
presentation source. Six libraries link; 527 compared API exports preserve
their types, bindings, visibility and symbol versions. A separate public-source
Linux D3D12/EGL compile-only workflow also passes. Nothing is loaded or installed.
Runtime, recovery and daily use remain unverified; counts stay
**4/8, 0/4, 3/3**. [Exact build scope and limits](MESA-LINUX-CROSS-COMPILE-2026-11-04.md).

**Source-only shutdown follow-up — November 4, 2026:** retirement now polls
the owned graphics thread before joining, and the relay command/state pair
shares one bounded deadline. CPU controls and both Windows owner compilations
pass; the owners are never executed. GPU/VM testing remains on hold.
[Changes, CPU evidence and remaining limits](RETIREMENT-POLLING-AND-QMP-DEADLINES.md).

**Normal-session diagnostic — November 3, 2026:** run 10 records 176 native
4K handoffs with a clean terminal prompt. Quickshell crashes during a Mesa
constant-buffer copy; the return to VGA stalls and containment forcibly stops
QEMU. Complete desktop, cleanup and daily use remain unverified; counts stay
**4/8, 0/4, 3/3**.
[Actual frame, captured stacks and tested limits](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md).

**Normal-session retirement follow-up — November 2, 2026:** the diagnostic
PID-file collision is removed from normal-session lifetime. Run 8 records
**432 native 4K GPU handoffs** and same-process actor retirement, but socket
resets and terminal draining still fail. No complete scene is captured and
daily use remains unverified. Counts stay **4/8, 0/4, 3/3**.
[Exact changes, CPU controls and retained failures](NORMAL-SESSION-RETIREMENT-WITNESSES.md).

**Normal-session frame transport — November 2, 2026:** the provisioned-account
session completes 61 native 4K GPU handoffs through the boot-console transition.
The actual capture contains only background/cursor, and shutdown fails. Full
wallpaper/application scenes and daily use remain unverified. Both strict host
builds and 135 CPU controls pass. Counts stay **4/8, 0/4, 3/3**.
[Actual capture, fixes and retained failures](NORMAL-SESSION-FIRST-GPU-FRAMES.md).

**Native renderer selection — November 2, 2026:** the normal session's factory
adapter excludes boot VGA from desktop rendering. The complete library builds
and 17 factory controls pass; guest/GPU integration remains unverified. Counts
stay **4/8, 0/4, 3/3**. [Exact source and controls](NATIVE-SESSION-RENDERER-SELECTION.md).

**Native boot console — November 2, 2026:** a rebuilt QEMU passes seven stopped,
windowless console controls. CPU ownership/redraw controls, actual launcher
argument checks and both strict host builds pass. Normal GPU boot and the real
display transition remain unverified.
Desktop acceptance remains **0/4**. [Implementation and tested limits](NATIVE-BOOT-CONSOLE.md).

**Root session service — November 2, 2026:** owner-aware admission, broker
activation and cleanup now pass real CPU/IPC controls. Native service tools
compile and a unit is provided; normal GPU boot and recovery remain unverified.
Desktop acceptance remains **0/4**.
[Implemented service, exact controls and remaining work](ROOT-SESSION-SERVICE.md).

**Normal-session startup boundary — November 2, 2026:** the persistent hook
now leaves watchdog information queries alone. A compositor exec wrapper
passes real credential/pidfd controls, and the staged desktop entry retains
the normal watchdog. Root activation and actual normal GPU login remain open.
[Exact startup contract, controls and remaining work](NORMAL-SESSION-STARTUP.md).

**Persistent compositor source — November 1, 2026:** a separate opt-in hook
preserves normal Hyprland arguments and submits damage-driven frames without
a finite workload. CPU controls and a complete pinned Aquamarine build pass;
normal-session GPU integration remains unverified. Counts stay **4/8, 0/4, 3/3**.
[Implementation, CPU/build evidence and remaining integration](PERSISTENT-COMPOSITOR-SESSION.md).

**Guest broker lifetime — November 1, 2026:** a root-supervised mode permits
rolling connections while preserving four/eight active-worker bounds. Real IPC
controls and the matched 1,800-handoff 4K three-app regression pass. The guest
session remains diagnostic; timing rises to 85.1 seconds against the 90-second
deadline, so performance stability remains open. Counts stay **4/8, 0/4, 3/3**.
[Exact scope, evidence and remaining product work](ROOT-BROKER-SESSION.md).

**Host session transport — November 1, 2026:** an explicit launcher-owned mode
permits idle connections while retaining absolute message deadlines and cleanup.
Both strict builds, real socket cancellation/backpressure controls and the full
1,800-handoff 4K three-app regression pass. The guest still has diagnostic limits;
normal persistent sessions and daily use remain open. Counts stay **4/8, 0/4, 3/3**.
[Implementation, hardware evidence and exact limits](LAUNCHER-HOST-SESSION.md).

**Original launcher profile — November 1, 2026:** run 30 completes the bounded
1,800-frame workload in 58.177667 seconds, with exact first/final 4K source/SDL
pixels and live cleanup. Both sandboxed Chromium GPU lifetimes and Nautilus now
have authenticated native-work correlation. Earlier tab, producer-cap and
fragmented-diagnostic failures remain recorded; targeted fixture/collector fixes
pass strict controls. Normal account/session, launcher activation, physical input,
sustained daily use and matched-native performance remain open. Counts stay
**4/8, 0/4, 3/3**. [Actual desktop/shader images, evidence and exact limits](ORIGINAL-PROFILE-APPLICATIONS.md).
[Argument/runtime contract](ORIGINAL-LAUNCHER-GPU-PROFILE.md).

**Go supervisor boundary — October 29, 2026:** the normal supervisor's internal
native branch validates and publishes QEMU's actual identity, retains its process
object, and signals the native owner during shutdown. Three hidden GPU workloads
pass the unchanged full data/pixel and cleanup checks; cancellation and controller
exit pass separately. Original input handlers remain unchanged. The normal guest
argument/runtime profile and activation remain open; counts stay **4/8, 0/4, 3/3**.
[Exact source boundary, process-chain evidence and limits](GO-NATIVE-GPU-SUPERVISION.md).

**Native launcher lifetime — October 29, 2026:** an inherited cancellation
capability, held creator process and exact QEMU identity message pass three
unchanged-image hidden GPU workloads. Separate early-lifecycle controls reap the
owned VM after cancellation or creator exit. No GPU mappings exist in those stop
controls; recovery during rendering remains open. Go launcher activation and
normal sessions remain incomplete; counts stay **4/8, 0/4, 3/3**.
[Protocol, actual process-chain tests and limits](NATIVE-LAUNCHER-LIFETIME.md).

**QEMU process ownership — October 29, 2026:** the native runtime binds a held
Windows process object to the expected QEMU identity and creator before resuming
the child. Hidden-child lifecycle controls and unchanged-image pressure/4K GPU
regressions pass. The launcher control protocol, parent-death recovery and normal
session integration remain open; counts stay **4/8, 0/4, 3/3**.
[Ownership checks and tested limits](QEMU-PROCESS-OWNERSHIP.md).

**Launcher argument boundary — October 29, 2026:** the extracted graphics
composer preserves arguments generated by the actual launcher for both cursor
preferences, with all 430 launcher sources unchanged. The refactored native bridge
passes the unchanged-image pressure and original 4K compute/shader regressions.
Launcher activation, process handoff, normal boot and ordinary input remain open;
counts stay **4/8, 0/4, 3/3**. [Source check and next integration boundary](LAUNCHER-GRAPHICS-ARGUMENTS.md).

**Vulkan buffer pressure — October 29, 2026:** two fresh guests recover from an
observed allocation refusal before entering the guest NVIDIA allocator, retry
the same 64 MiB GPU buffer, and finish both original full GPU data rounds. All
13 submissions and 20 hardware-queue signals retire; native objects and mappings
clean up before VM exit. FD-zero, FD-exhaustion, corruption rejection and both
original 4K workloads pass. This is advisory single-process external-buffer
admission; concurrent pressure, general desktop recovery and daily use remain open.
Counts stay **4/8, 0/4, 3/3**. [Exact scope and retained failures](VULKAN-GPU-PRESSURE-RECOVERY.md).

**Typed Vulkan GPU data — October 28, 2026:** four fresh guests each pass two
complete 64 KiB GPU export/free/import/readback rounds using Windows-native buffer
metadata, including FD zero and FD-exhaustion rollback/retry. A real GPU corruption
is rejected. Both original 4K compute and shader workloads pass after correcting
a retained broker build-profile failure. Earlier ownership leaks, native import
failures and a forced-stop wait hang stay recorded. Daily use remains unaccepted;
counts stay **4/8, 0/4, 3/3**. [Exact data proof, limits and failures](VULKAN-GPU-BUFFER-ROUNDTRIP.md).

**Vulkan buffer/FD ownership — October 28, 2026:** two fresh guests each pass
32 unique close-on-exec exports from a 64 KiB GPU-only buffer, including FD
lifetime after the creator's memory is freed. Separate native buffer admission
and Linux Dozen ownership fixes preserve the original failure controls. Both
complete 4K compute rounds still pass, with byte guards unchanged and native
resources/mappings retired before VM exit. Subsequent scoped GPU import/readback checks are recorded above.
Vulkan rasterization/windows and daily-use acceptance remain open. Counts stay
**4/8, 0/4, 3/3**. [Exact matrix and source checks](VULKAN-MEMORY-FDS.md).

**4K Vulkan compute — October 28, 2026:** two fresh guests each pass two shader
rounds with every one of 8,294,400 output words verified. A GPU corruption
control fails at exactly the final word. Selecting the existing 256-slot CPU
aperture clears the retained 128-slot readback refusal without changing byte
guards. That refusal also retains four reservations until VM exit; recovery
remains open. This is offscreen compute, with no Vulkan rasterization or window
presentation acceptance. Counts stay **4/8, 0/4, 3/3**.
[Full integer checks, GPU controls and retained failures](VULKAN-4K-COMPUTE.md).

**GPU waits and Vulkan initialization — October 28, 2026:** six background GPU
wait controls pass, including ANY/ALL, descriptor reuse and capacity; all 69
notifications retire normally. A separate process-exit control cancels its
kernel wait but retains native mappings until VM exit, so live crash recovery
remains unverified. Original Dozen also reaches Vulkan device creation and idle
with the same opt-in bridge; those initialization runs do not cover shaders.
GPU-backed window presentation remains open. Counts stay **4/8, 0/4, 3/3**.
[Wait controls and retained recovery failure](BACKGROUND-ASYNC-GPU-WAITS.md) ·
[Vulkan baseline/candidate comparison](VULKAN-DEVICE-BRINGUP.md).

**Three GPU applications — October 27, 2026:** private run **245** completes
**1,800 handoffs** with the shipped Nautilus file manager, sandboxed Chromium
and terminal visible together. GTK uses a realized GL renderer with correlated
native work. Browser interactions, resize, close/reopen and live cleanup pass;
complete first/final pixels have maximum error one. A paired owner/controller
agreement expands allocation metadata to 2,048 records, with 1,061 used and all
byte budgets unchanged. Earlier allocation refusals and integration failures
remain retained. Daily-use acceptance stays open; counts remain **4/8, 0/4, 3/3**.
[Actual three-app frame, agreement and retained failures](APPLICATION-OBJECT-CAPACITY.md).

**Background 4K shader pair — October 27, 2026:** original and sampled-read
guest shims each render two actual 3840×2160 NVIDIA shader frames with complete
pixel checks, native shared-texture handoff and live cleanup. Corresponding
captures are byte-identical; every pixel passes the existing integer oracle,
with maximum channel error one. Presentation and input are disabled for these
background runs. Diagnostic counts fall from 4,470,000 to 894 reads; performance
and daily-use acceptance remain open. Counts stay **4/8, 0/4, 3/3**.
[Actual shader frame, source guards and scope](BACKGROUND-4K-SHADERS.md).

**Actual browser resizing — October 27, 2026:** private run **239** completes
**1,800 handoffs**, both browser sessions' window/viewport resize and restore,
controlled CDP interactions, reopen and live cleanup. GPU processes, compositing
and original shaders survive all four changes. Complete first/final pixels pass
within the existing tolerance of two. Atomic owned interaction/resize proofs
avoid reliance on journal-write contiguity. Separate Windows input trial 238
remains rejected after the host UI tool cannot observe the window; no input was
sent. Official browser input, longer sessions, broader apps and product acceptance
remain open. Counts stay **4/8, 0/4, 3/3**.
[Actual post-resize frame, retained input trial and scope](GPU-BROWSER-RESIZE.md).

**Controlled browser interactions — October 27, 2026:** private run **237**
completes **1,800 handoffs**, terminal input/resize/focus, browser close/reopen,
complete first/final pixels and live cleanup. In both browser sessions, owned
CDP wheel events scroll the real page down 500 pixels and back, text insertion
returns the expected input value, and a temporary tab opens/closes. The GPU
process, compositing and original NVIDIA shader survive. These checks do not
certify official browser input or physical latency. A journal diagnostic
collision remains retained; the audit uses intact owned-file controller copies.
Counts stay **4/8, 0/4, 3/3**.
[Actual final frame, exact evidence and remaining work](GPU-BROWSER-INTERACTIONS.md).

**GPU browser lifecycle — October 27, 2026:** private run 234 completes all
**1,800 handoffs**, terminal typing and cursor response, three terminal sizes,
four focus checkpoints, browser close/reopen and live cleanup before natural
VM exit. Both browser sessions pass HTTPS, NVIDIA shader, GPU compositing and
all 17 observed GPU-thread sandbox checks. Complete first/final source-to-SDL
pixels pass with maximum channel errors two and one. Test startup synchronization
changed; official input did not. Ordinary scrolling/typing/tabs, normal launcher,
long sessions, pressure recovery and daily-use acceptance remain open.
Counts stay **4/8, 0/4, 3/3**.
[Actual final frame, retained failures and scope](GPU-BROWSER-LIFECYCLE.md).
Follow-up **235** verifies the browser's floating size and position on both
launches, exposes the shipped wallpaper and passes all **1,800 handoffs** again,
including complete first/final pixels, input and live cleanup. Browser interaction
and product acceptance remain open. [Actual wallpaper/browser/terminal frame](GPU-BROWSER-LIFECYCLE.md#floating-browser-with-visible-wallpaper--follow-up-run-235).

**First GPU-composited browser frame — October 26, 2026:** a readable HTTPS page
and NVIDIA shader are visible in Omarchy with GPU compositing enabled and all
17 observed GPU threads sandboxed. Full first-frame source/SDL pixels pass.
The input/sustained workload still fails after two handoffs; normal launcher and
daily use remain open. Counts stay **4/8, 0/4, 3/3**.
[Actual 4K source and exact boundaries](GPU-COMPOSITED-BROWSER-FRAME.md).

**Sandboxed browser shader — October 26, 2026:** Chromium loads a secure public
HTTPS page and runs an actual NVIDIA/ANGLE/D3D12 shader with all 16 observed GPU
threads sandboxed. The captured browser page body is missing, GPU compositing
reports disabled_software, and only two handoffs complete. This is shader/sandbox
progress; browser and daily-use acceptance remain open. Counts stay **4/8, 0/4,
3/3**. [Actual incomplete frame and exact evidence](SANDBOXED-BROWSER-SHADER.md).

**Application memory — October 26, 2026:** an explicit owner/controller agreement
admits a shared 4 GiB reported GPU-usage pool after checking current native
headroom. The public-source enabled desktop passes **1,800 handoffs**, 5,731
submissions, 5,724 successful health queries, full source/SDL pixels, input,
resize, focus, application replacement and live cleanup. Its zero-eviction
workload lacks retained-residency coverage; a separate default-off regression
passes the historical positive eviction/reuse/reclaim checks. A private browser
attempt exceeds the old 1 GiB limit without refusal but still fails browser
acceptance. Counts remain **4/8, 0/4 and 3/3**.
[Actual frame, earlier failures, opt-in contract and exact limits](APPLICATION-MEMORY.md).

**Application compatibility — October 25, 2026:** negotiated process and resource
capacity clears Chromium's initial admission failures across host and guest.
The legacy networked desktop passes another 1,800-handoff full-pixel regression.
Browser navigation and scanout-usage allocation remain unresolved; browser,
normal-launcher and daily-use acceptance stay open. Counts remain 4/8 and 0/4.
[Actual frame and retained browser attempts](GUEST-BROWSING-PROGRESS.md#application-capacity-follow-up).

**Native GPU health — October 25, 2026:** the published-runtime build completes
**5,786 real device-health queries and 1,800 GPU handoffs**, including application
close/relaunch, complete source/SDL pixels and live cleanup. Native errors and
fault states are preserved. A separate private rollback experiment crossed the
1 GiB guard and failed; it remains excluded. Progress counts stay unchanged.
[Actual frame, validation and retained failure](GPU-DEVICE-EXECUTION-STATE.md).

**Residency recovery investigation — October 24, 2026:** a controlled
single-allocation refusal recovered through 1,800 GPU handoffs, but a
four-allocation repeat caused device loss. That runtime candidate stays
unpublished. The failure is retained with 16 replay checks; private paging-queue
research continues and progress counts stay unchanged.
[Actual frame, rejected candidate and next check](LIVE-RESIDENCY-ROLLBACK-INVESTIGATION.md).

**Verified metadata checkpoint — October 24, 2026:** immutable metadata requests fall
from **54 to 12 across six controlled validations**, while mutable counters
stay fresh. A new **1,800-handoff 4K GPU desktop regression** passes complete
pixels, two application windows, input response, resizing and live cleanup.
Atomic diagnostic receipts fix a real journal-prefix collision. Rendering
speedup and memory-pressure recovery remain unverified; two earlier pressure
failures stay rejected. [Actual frame, retained failures and exact scope](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-METADATA-AND-TRACE-FRAMING.md).

**Earlier two-window checkpoint — October 23, 2026:** two fresh guests each pass
**1,800 GPU handoffs over 33.52 / 33.03 seconds**, with two readable NVIDIA
application windows, complete source/SDL pixels, four focus checkpoints,
independent changing counters and live native cleanup. Explicit profile 14/v9
opts into a **384 MiB aggregate CPU mapping guard**; the GPU usage guard stays
1 GiB and older agreements retain their limits. A separate frozen v6 desktop
regression also passes on the new binary. An earlier **16-reference alias
admission failure remains unresolved**. This is a bounded fixture result;
normal launcher, continuous stability and desktop product acceptance remain
open. Progress counts are unchanged.
[Actual two-window frame, precise failures and exact sources](LIVE-MULTIAPP-CPU384.md).

**Recovery follow-up:** a valid `ENOMEM`/`ENOSPC` resolution reply now preserves
the broker connection and live caller FD. Two fresh guests additionally recover
from one explicitly injected **guest-side refusal before the native alias call**,
retry the exact GPU resource, and complete **1,800 handoffs** with complete
source/SDL pixels, input/resize response and live cleanup. An initial origin-label
defect and a run occluded by a real calendar popup stay nonaccepted. The unchanged
wallpaper oracle is retained. Actual native quota-refusal recovery and the natural
reference-limit cause remain open; progress counts stay unchanged.
[Actual post-fault frame, every attempt and exact limits](LIVE-INJECTED-ALIAS-GPU-RECOVERY.md) ·
[Original transport correction](LIVE-ALIAS-CAPACITY-RECOVERY.md).

**Earlier multi-window admission investigation — October 23, 2026:** a second real
application initializes its NVIDIA OpenGL renderer. The simultaneous workload
then reaches the unchanged 192 MiB CPU mapping guard and fails native cleanup
verification, before a desktop frame completes. It is **rejected**. New opt-in
lease/object agreements retain older limits, and the candidate separately passes
the frozen v6 **1,800-frame 4K desktop regression**, complete pixels, QMP input
response, resizing and live cleanup. Multi-window rendering and admission-failure
recovery remain open; the progress counts are unchanged.
[Actual compatibility frame, precise failures and source evidence](LIVE-MULTIWINDOW-ADMISSION.md).

**Latest GPU address-pressure checkpoint — October 22, 2026:** two fresh guests
exceed **2 GiB of virtual GPU mappings** before frame one, verify 56 GPU copy/evict
cycles and retained contents, then pass **1,800 desktop handoffs each over 35.3
and 34.5 seconds**. Complete 4K source/SDL pixels, rendered input response,
lifecycles, resizing and live cleanup pass. The reported resident and CPU
mapping guards are unchanged. This closes the bounded extended-address check;
longer memory-pressure recovery, continuous multi-window use and normal launcher/input
acceptance remain open. [Actual frame, exact evidence and scope](LIVE-GPU-VA-PRESSURE.md).

**Earlier managed-residency checkpoint — October 22, 2026:** two fresh guests
pass 1,800 GPU handoffs each over **32.9 and 32.2 seconds**, complete pixels,
input response, lifecycles, resizing and live cleanup. The ownership ledger
accounts for all 47/45 deferred references with no retained references left.
A new explicit address-accounting agreement separates owned GPU reservations
from the unchanged resident-memory guard. Those earlier successful runs do not
exercise the extended virtual boundary; the new bounded follow-up above does. Default
behavior and earlier failed attempts are retained.
[Actual frame, all attempts and scope](LIVE-MANAGED-GPU-RESIDENCY.md).

**Earlier sustained rendering checkpoint — October 22, 2026:** two fresh guests
pass **1,800 GPU handoffs over 33.5 and 33.8 seconds**, complete 4K source/SDL
pixel audits, rendered input response, application lifecycles, resizing and live
cleanup. Matching private libraries from the signed installed NVIDIA **621.01**
driver restore graphics initialization after a host driver update. The official
input implementation is preserved. A shared-budget prototype failed an eviction
path and remains rejected; the verified host source is retained. Displayed cadence,
physical input latency, normal launcher integration and ≥93% native performance
remain unverified.
[Actual frame, all attempts and limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-SUSTAINED-NVIDIA-DESKTOP.md).

**Earlier live trace comparison:** moving the complete graphics diagnostics from the
emulated UART to a dedicated virtio-serial port reduces median steady GPU handoff
intervals from **43–44 ms to 16.6–16.7 ms** in this bounded workload. Two virtio
runs and two UART baselines pass 300 handoffs, full 4K source/SDL pixel audits,
input-response checks and live cleanup. A separate UART input-response failure
remains rejected. Official input and the GPU protocol are unchanged. These CPU
intervals do not establish displayed FPS, physical input latency or 93% native
performance.
[Actual SDL frame, complete traces and comparison limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-VIRTIO-GRAPHICS-TRACE.md).

**Latest display follow-up:** four active-workload runs pass 300 handoffs,
exact complete 4K source pixels, independently scaled full SDL window pixels and
live GPU cleanup. Two use the original bridge; two use gathered GPU replies and
TCP_NODELAY, including the separate instrumented QEMU run. The timing comparison establishes no performance gain. A setup-guard
failure and an all-zero SDL window capture remain rejected; the next window
capture passes. The cause of the zero buffer and earlier frame-207 device loss
remain unconfirmed. Earlier frame intervals include a 50 ms repaint sleep.
Physical scanout/input latency and near-native performance remain unmeasured.
[Actual image, reconstructable repeats and limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-ACTIVE-SDL-TRANSPORT.md).

**Failure teardown follow-up:** a controlled frame-64 interruption reproduced a
secondary SDL consumer-loss error. Draining the SDL output before stopping QEMU
now retires all accepted frames and releases native objects while preserving the
original error. A separate fatal control verifies owned-VM exit before host abort.
The corrected bridge also passes a normal 300-frame full-pixel regression.
[Before/after evidence, actual rendering and remaining limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-SDL-FAILURE-TEARDOWN.md).

**Latest guest follow-up:** two fresh guests pass **300 GPU handoffs with real
application resizing**, independently verified enlarged/restored scene pixels,
QMP input response and live cleanup. Two earlier 300-frame input-response
baselines remain separate. [Images, raw evidence and scope](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/LIVE-UWSM-SUSTAINED-RESIZE.md).
The product should preserve the official keyboard/cursor code and QEMU SDL
input path. Forty input-related files are unchanged; the real guest is now
connected to SDL. The user reports cursor movement, with automated full-input
and normal-launcher acceptance still open. [Source-preservation record and integration boundary](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).
The staging trims still occur after the streams; hardware reallocation while
rendering remains open. The earlier results below keep their own scope.


Two fresh guests pass **UWSM in a real PAM/logind session**.
Independent supervisor queries verify the active compositor service, exact
MainPID/cgroup and active graphical session target. Each passes **five NVIDIA application launch/close cycles**
while the same Hyprland compositor keeps running. The real Omarchy 4.0.4
desktop is captured at **3840×2160** with shipped wallpaper, Quickshell bar
and a readable GPU application. Every actor, including the primary compositor,
is reclaimed before native disconnect; every backing mapping is reclaimed before VM exit.
Twelve GPU handoffs, complete first/last
backbuffer audits, natural shutdown and native cleanup pass per run.
Demand-allocated staging avoids preparing about 64 MiB at startup while retaining all
three ring slots. Both runs stay below the unchanged 1 GiB reported-GPU-usage guard.
Hyprland also enumerates QEMU's keyboard and tablet through the guest input seat.
The later **run 93** verifies a fixed QMP keyboard sequence in the real Alacritty
PTY, the expected pointer motion and the terminal's green confirmation in the
complete NVIDIA backbuffer. Its fresh **repeat 94 fails** the unchanged 1 GiB
GPU-usage guard during shutdown and requires VM termination. This is one accepted
input-response diagnostic, not repeated stability. Physical Windows-window
keyboard/mouse delivery remains unverified.
This bounded repeat does not establish long-run memory stability.

**The subsequent staging-reclamation change passes two fresh guests (95–96).**
Each passes the complete QMP input-response, 4K GPU pixels and live-cleanup audit
under the same usage guard, after actually releasing 31.875 MiB of idle staging.
Both GPU copy fences retire before release; three-slot capacity is preserved.
Neither run reallocates a trimmed slot during rendering. Continued hardware
trim/reallocation and longer workloads remain required.
[Two-run evidence, actual frame and scope](https://github.com/7Wdev/try-omarchy-windows/blob/bf60d2848f831be892f8c5e69b686bcedb98644e/docs/LIVE-UWSM-STAGING-RECLAIM.md).

[Actual UWSM image, reproducible evidence and retained failures](https://github.com/7Wdev/try-omarchy-windows/blob/2abc463db5cdb8c2b62b3c0526fe60932f9acde7/docs/LIVE-UWSM-NATIVE-INPUT-DISCOVERY.md)
bind every application and the shell to authenticated NVIDIA renderers, real
resource FDs and independent recipient NT references. The
[earlier CPU-scan removal checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/cb505099ef461603c45ccc8b322031546966ee36/docs/LIVE-OMARCHY-GPU-LIFECYCLES.md)
measures 0.28–0.53-second application retirement while keeping complete frame audits. The 192 MiB CPU mapped-data
and 1 GiB GPU-VA/reported-usage guards are unchanged. Ten ordinary frames per
run avoid CPU upload/readback; two use diagnostic GPU readback.

**The first GPU-rendered 4K Omarchy/Hyprland frame is achieved.**
[Actual input-response image, raw evidence and failed repeat](https://github.com/7Wdev/try-omarchy-windows/blob/0774e40f0dd394aedb90a5d341b87cc42a00cc91/docs/LIVE-UWSM-QMP-INPUT-RESPONSE.md)
preserve the scope of that later checkpoint.

**3/3 diagnostic scene elements** means wallpaper, bar and readable GPU
application are present in the accepted 4K captures. **4/8 backend stages** and
**0/4 desktop product gates** retain their original acceptance definitions.
These are different checklists, not engineering-effort percentages. Capturing
the desktop does not establish normal launcher presentation or interactive use.

The earlier Windows diagnostic client area is 1280×720; the new real-guest SDL
backbuffer is 2560×1566 with a 3840×2160 GPU source. Physical 4K scanout, all-frame
display, normal first-boot/launcher/input, long-run stability and performance are
unverified. Both passing UWSM runs have **zero mappings or native children deferred to VM exit**.
Earlier usage-guard and primary-reservation failures remain retained and rejected.
GPU memory pressure, physical Windows input, normal launcher integration, continuous multi-window presentation, longer workloads
and recovery are next. The [earlier capture checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-REFERENCE-FIX.md)
retains its original scope and failures.

## Backend verification stages

| Stage | Status | Evidence or exit criteria |
| --- | --- | --- |
| 1. Fork and upstream maintenance | Demonstrated | Preserve upstream history and fixes on `master`; review experimental work in [PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2). |
| 2. Windows bridge and D3D12 initialization | Demonstrated in the lab | Genuine Linux NVIDIA runtime initializes through QEMU and creates owned Windows KMT objects. |
| 3. 4K guest shaders and shared GPU textures | Demonstrated in the lab | [Independent 4K shader pixels, GPU copies and fence retirement](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-4K.md). |
| 4. Repeated guest rendering | Demonstrated in bounded diagnostics | [64-frame guest runs](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-GUEST-STREAM.md), safe allocator/texture reuse and explicit cleanup. This is not long-run desktop or all-frame display acceptance. |
| 5. Guest graphics/device integration | In progress | Native render-node/GBM, EGL fences, separate GPU clients, 4K captures and five application lifecycles with live reclamation pass twice. Supported device integration, recovery and sustained work remain required. |
| 6. Interactive Omarchy/Hyprland desktop | In progress: real guest → QEMU SDL, two 1,800-handoff 4K repeats with input response, resizing and live cleanup | Validate normal launcher sessions, displayed cadence, official physical input, focus, fullscreen and simultaneous windows. |
| 7. Required APIs and NVIDIA features | Pending acceptance | OpenGL diagnostics pass; test general OpenGL, Vulkan, CUDA, video, NVIDIA DLSS and hardware ray tracing in real guest applications. |
| 8. Reliability and ≥93% native performance | Pending acceptance | Matched guest/native benchmarks through 4K, concurrent host/guest GPU workloads, long runs, bounded memory and device/VM recovery. |

NVIDIA **DLSS** and **hardware ray tracing** are required, unverified targets.
Their acceptance requires actual guest applications or SDK workloads with
verified NVIDIA hardware execution, correct output and measured performance.

## Desktop product acceptance gates

**0/4 complete.** The first three gates have partial diagnostic evidence and are
shown in amber. A verified image is a real milestone within these gates; it does
not close the broader product requirements below.

| Gate | Current evidence | What closes the gate |
| --- | --- | --- |
| Guest graphics device, buffers and synchronization | In progress: separate NVIDIA clients, native resource FDs, GPU waits and five application lifecycles pass twice; every backing mapping is reclaimed live. | A supported guest device path with correct sustained ownership, synchronization and recovery beyond the bounded diagnostic. |
| First Hyprland frame in the Windows app | In progress: actual 4K wallpaper, bar and readable GPU application captured twice. Separate diagnostic presentation is not normal launcher acceptance. | Boot the normal guest session, independently verify a correctly displayed compositor frame in the Windows app, and publish its image in PR #2. |
| Usable interactive desktop | In progress: two bounded QMP keyboard/pointer and GPU-rendered application responses pass after staging reclamation. Physical Windows input and continuous multi-window operation are unverified. | Continuously display real apps and multiple windows with working input, focus, resizing and fullscreen through 4K. |
| Stable and performant product | Pending acceptance: no matched near-native result or long desktop run has passed. | Sustain workloads, recover from failures, keep memory bounded and meet ≥93% of matched native performance. |

## Remaining work

1. Extend the bounded pre-stream address-pressure result to memory pressure
   while rendering, hardware staging reallocation and longer workloads,
   retaining official input and the independent physical-memory guards.
2. Integrate the supported guest device path, normal first-boot session and Windows
   launcher/input, with independently verified continuous presentation.
3. Extend the bounded live-retirement result to concurrent application windows,
   longer workloads and failure recovery.
4. Exercise multiple applications, resize/fullscreen, longer runs and bounded
   whole-process memory use through 4K.
5. Benchmark matched native/guest workloads and concurrent Windows/guest GPU use.
6. Implement and validate the required Vulkan, CUDA, video and NVIDIA features
   with real applications. A D3D12 or OpenGL diagnostic does not establish them.

There is no defensible completion date yet. Stage counts cannot estimate the
duration of the remaining driver, compositor and product work.

## Evidence history

Earlier results keep their original scope and failures:

- [Actual QEMU SDL NVIDIA host relay, BGRA correction and retained pixel failure](https://github.com/7Wdev/try-omarchy-windows/blob/6db01ffa69db405f9c76fb51da335bcf0513e8f6/docs/LIVE-QEMU-SDL-GPU-RELAY.md).
- [Idle staging reclamation and two passing 4K input-response runs](https://github.com/7Wdev/try-omarchy-windows/blob/bf60d2848f831be892f8c5e69b686bcedb98644e/docs/LIVE-UWSM-STAGING-RECLAIM.md).
- [QMP input delivery and actual 4K GPU response, including the failed repeat](https://github.com/7Wdev/try-omarchy-windows/blob/0774e40f0dd394aedb90a5d341b87cc42a00cc91/docs/LIVE-UWSM-QMP-INPUT-RESPONSE.md).
- [Readable 4K captures and the earlier admission failure](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-READABLE-GPU-DESKTOP.md).
- [Wallpaper/application partial capture and slot-exhaustion findings](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-DESKTOP-CONTENT.md).
- [GPU shell-bar checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-GPU-SHELL.md).
- [Real Omarchy root boot with a read-only base and RAM overlay](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-NORMAL-OMARCHY-ROOT-BOOT.md).
- [300-frame real Wayland scene, with a software-rendered Foot client](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-HYPRLAND-WAYLAND-SCENE.md).
- [Earlier compositor capture](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-HYPRLAND-COMPOSITOR-CAPTURE.md),
  [GBM rendering](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-GBM-NATIVE-RENDER.md) and
  [EGL native synchronization](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-EGL-NATIVE-SYNC.md).

Historical 256-object/startup-only limits and missing-shell/application findings
describe their original experiments. They are not the current 4K capture scope.
The [draft PR](https://github.com/7Wdev/try-omarchy-windows/pull/2) retains the
implementation, images and detailed verification history.
