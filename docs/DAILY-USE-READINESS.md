# Daily-use readiness

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

**Normal GPU transport follow-up, November 2:** the real account/session submits
61 native 4K frames after the boot-console transition. The screencopy contains
only background/cursor; shutdown fails before SDL detach completes. Full scene,
cleanup/recovery and daily-use acceptance remain open. Counts stay **4/8, 0/4,
3/3**. [Exact observations and retained failures](NORMAL-SESSION-FIRST-GPU-FRAMES.md).

**Normal-account boot follow-up, November 2:** official provisioning and the
SDDM/PAM/UWSM broker path now execute in four inactive-desktop tests. Environment
corrections reach a native 4K swapchain, but the first persistent output fails.
No normal desktop frame or daily-use acceptance is established. Counts remain
**4/8, 0/4, 3/3**. [Retained failures, fixes and cleanup observations](NORMAL-SESSION-BOOT-ATTEMPTS.md).

**Root activation, November 2:** the service now authenticates and registers the
configured compositor and passes CPU lifecycle controls. Guest bundling, normal
GPU boot, sustained use and recovery remain unverified; acceptance stays **0/4**.
[Exact implementation and tested limits](ROOT-SESSION-SERVICE.md).

**Normal-session startup, November 2:** the watchdog query regression is fixed;
a root-authenticated exec client and native desktop-entry stage pass CPU controls.
The production root service and normal GPU session remain incomplete. Desktop
acceptance remains **0/4**. [Exact evidence and next work](NORMAL-SESSION-STARTUP.md).

**Persistent compositor source, November 1:** the timed fixture now has a separate
persistent output implementation with passing CPU controls and a full Aquamarine
build. It is not yet connected to the normal provisioned-owner session and has
no persistent hardware result. Desktop acceptance remains **0/4**.
[Exact scope and remaining work](PERSISTENT-COMPOSITOR-SESSION.md).

**Broker follow-up, November 1:** the matched root-supervised broker and host pass
the bounded 4K workload. Normal sessions remain incomplete, and the latest
85.1-second workload leaves timing stability open. [Evidence and limits](ROOT-BROKER-SESSION.md).

**Host-session follow-up, November 1:** an opt-in launcher-owned host transport
passes real idle/backpressure controls and the complete bounded 4K application
regression. Guest lifetime limits, normal startup, physical input and daily-use
acceptance remain open. [Exact scope and remaining integration](LAUNCHER-HOST-SESSION.md).

**Latest November 1 follow-up:** bounded run 30 passes 1,800 handoffs in
58.177667 seconds, exact first/final 4K pixels, authenticated native work for
both Chromium GPU lifetimes and Nautilus, and live cleanup. Tab retirement,
producer lifetime and coherent diagnostic-output fixes retain earlier failures.
Normal sessions, launcher activation, physical input, long sessions and daily
use remain open; counts stay **4/8, 0/4, 3/3**.
[Current desktop/shader captures and precise proof](ORIGINAL-PROFILE-APPLICATIONS.md).
The older observations below retain their original scope.

**October 31 launcher follow-up:** the original argument profile now boots a
private guest and reaches an actual 4K NVIDIA desktop frame, with exact complete
source/SDL pixels. It completes only three handoffs, leaving the existing
1,800-frame regression rejected. Normal account/session flow, activation,
official physical input and sustained operation remain unverified.
[Actual first frame, fixes and retained failures](LAUNCHER-GUEST-GRAPHICS-BOOTSTRAP.md).

The NVIDIA backend remains an engineering prototype. A complete 4K GPU desktop
with wallpaper, bar and application windows is verified, including input response,
application replacement, real device-health queries and live resource cleanup.
The latest public-source networked regression completes 1,800 handoffs with an
explicit 4 GiB application-memory agreement. A separate default-off regression
retains positive eviction/reuse coverage; the larger pool produces zero evictions
in this workload. [Actual frame and exact limits](APPLICATION-MEMORY.md). This does not
establish daily-use stability or compatibility with arbitrary applications.

## Next practical acceptance: Chromium

Chromium is already present in the guest image. The new SDL QEMU candidate includes
libslirp and passes CI/local virtio NIC realization checks. An explicitly networked
read-only guest passes DHCP, DNS and certificate-verified public HTTPS, followed by
the complete 4K GPU desktop workload. Preserving the source root's permissions in
the RAM overlay fixes an observed resolver service failure. [Actual frame and
networking evidence](GUEST-BROWSING-PROGRESS.md).

Explicit process and resource-reference agreements now clear the observed
four-process and sixteen-reference startup boundaries. Host, guest broker and
client ioctl tracking use the same authenticated agreement; default behavior and
older aggregate memory budgets are preserved. A new explicit memory agreement
clears the old 1 GiB reported-usage boundary. A private follow-up now displays a
readable public HTTPS page and NVIDIA shader with GPU compositing enabled and
all 17 observed GPU threads sandboxed. A later private run now completes 1,800
handoffs, complete first/final 4K source-to-SDL pixels, terminal typing/cursor
response, terminal resize, focus checkpoints and browser close/reopen with live
GPU retirement. Both browser sessions retain the shader/compositing/sandbox
checks. The fixture waits for browser readiness before starting the existing
30-second terminal input timer; official input code is unchanged. Browser
keyboard/wheel routing, continuous browsing and longer sessions remain unaccepted.
[Actual final frame and exact limits](GPU-BROWSER-LIFECYCLE.md).

A further 1,800-handoff private trial passes controlled browser wheel, text and
tab checks in both sessions through owned CDP pipes, with the GPU process,
compositing and shader preserved. Complete final pixels and live cleanup pass.
These direct browser checks do not certify ordinary SDL/OS browser input or
physical latency. [Actual interaction frame and precise limits](GPU-BROWSER-INTERACTIONS.md).

The resize follow-up passes both sessions' actual compositor geometry and browser
viewport changes, with the original GPU process, compositing and shader retained,
then completes 1,800 handoffs with full pixels and live cleanup. A separate Windows
input trial cannot inspect the host window, sends no input, and remains rejected.
Ordinary Windows/SDL browser input and latency still need evidence.
[Actual post-resize frame and retained failure](GPU-BROWSER-RESIZE.md).

A later interactive fixture now keeps GPU handoffs flowing during the input
check, clearing an observed hold at frame 1,799. The owned browser has focus,
but keyboard delivery still fails; run 249 preserves that rejection and verifies
its actual final-frame drain and live cleanup. Background queue controls and
strict controller compilation now run without opening a VM or desktop window.
[Continuous-display evidence and remaining input failure](CONTINUOUS-INPUT-PROGRESS.md).

The browsing test must establish all of the following:

- A normal Wayland Chromium window with its sandbox retained and no software
  GPU fallback, correlated with the actual NVIDIA GPU process and native work.
- Successful DNS, TLS and public HTTPS-page loading through the guest network.
- Correct text, scrolling, resizing, tabs, ordinary input and window switching.
- WebGL/canvas rendering evidence, with software renderers rejected.
- Browser close/reopen and native resource retirement while the desktop remains
  responsive. Video playback and hardware video decoding need separate evidence.

## Remaining product acceptance

The next bounded integration passes **1,800 handoffs** with Nautilus, Chromium
and the terminal visible together, browser close/reopen and live native cleanup.
A new paired allocation-metadata agreement clears the observed 1,024-record
boundary without changing byte limits; the workload reaches 1,061 records.
Complete first/final source-to-SDL pixels pass. Earlier three-app failures and
the missing optional retirement journal print remain documented. This broadens
tested application coverage; it does not certify arbitrary GTK applications or
long daily sessions. [Actual three-app frame and exact limits](APPLICATION-OBJECT-CAPACITY.md).

Browser success is one application workload. Daily-use readiness also requires a
normal Omarchy account/session and launcher flow, broader application and window
coverage, longer interactive sessions, suspend/reopen behavior where supported,
and reliable failure handling under memory pressure. The private residency
rollback experiment remains excluded after device-loss failures.
An observation-only browser repeat using the published host crosses the existing
GPU-usage guard and requires an owned VM stop. Native cleanup succeeds after
exit, but live recovery remains unverified. A new owner-only pre-native admission
pilot avoids that forced stop in one repeat: the guest receives ENOMEM and the
VM exits naturally with live cleanup. Chromium restarts its GPU process and
navigation still fails. The pilot uses mapping-size estimates with uncovered
cases; it is not a complete pressure-recovery solution.
[Policy and retained hardware observations](MAPPED-RESIDENCY-ADMISSION.md).

The official keyboard/cursor implementation and launcher reserved-key route remain
the required input path. Physical input latency, physical scanout cadence and
matched-native performance must be measured. Backend stage counts (4/8) and
desktop product gates (0/4) are checklists, not percentages of engineering effort.

The native bridge has an inherited cancellation capability, held creator monitor
and exact owned-QEMU identity message. The Go supervisor now consumes that
identity through an internal prepared-profile branch and publishes the actual
QEMU PID for its existing window/bridge checks. Hidden GPU and early lifecycle
controls pass. Original input handlers remain unchanged; only `main.go`'s
supervision boundary changes among the original 430 launcher files. The matching
normal guest graphics bootstrap, activation and recovery during rendering
remain open. The original-argument runtime profile and early identity handoff
now pass CPU/helper tests. First real VM attempts reach binding but expose a QEMU
socket reconnect crash before boot; its upstream backport passes local transport controls.
[Failure and required verification](QEMU-SOCKET-RECONNECT.md). Successful normal
VM boot remains unverified.
[Implementation and precise tested scope](ORIGINAL-LAUNCHER-GPU-PROFILE.md).

A new default-off guest build option samples mapped-memory diagnostics twice
instead of 10,000 times. Strict shims, transport fault tests and real Linux IPC
mapping controls pass while retaining the original retirement/MMIO checks.
The paired display-free clear/readback check now passes its scoped broker, GPU
retirement, pixels and cleanup audit. Desktop integration and performance
measurement are still required. [Implementation and verification scope](DIRECT-READ-DIAGNOSTICS.md).

The next background pair verifies two actual 4K shader frames per selection,
typed native GPU handoff, complete pixels and live cleanup, with identical images
across the original and sampled shims. [Actual 4K shader image](BACKGROUND-4K-SHADERS.md).
This adds GPU coverage while preserving the open desktop/product gates.

The full goal still includes broader OpenGL/Vulkan/CUDA/video support, DLSS,
hardware ray tracing and at least 93% matched-native performance. These are not
certified by the current desktop or browser fixtures.
