# Persistent compositor session source

**Root-service follow-up:** owner-aware admission and broker orchestration now
pass CPU controls. Fatal frame errors also request one ordinary SIGTERM after
fence cleanup. Real normal GPU startup and fault retirement remain unverified.
[Current service and precise scope](ROOT-SESSION-SERVICE.md).

**November 2 follow-up:** watchdog queries now bypass GPU initialization, and a
normal compositor exec target plus desktop-entry stage pass CPU/IPC controls.
The root activation service and actual normal GPU login remain open.
[Startup contract and scoped results](NORMAL-SESSION-STARTUP.md).

**November 1, 2026:** a separate opt-in Hyprland entry point and Aquamarine
adapter now build without a finite scene, frame-count exit, repaint thread or
replacement compositor arguments. CPU controls pass; the full pinned Aquamarine
library links. This component has **not run a persistent GPU desktop**. Backend
and desktop counts remain **4/8, 0/4**, with the earlier diagnostic scene at **3/3**.

## Implementation

`experimental/windows-native-gpu/hyprland_session.c` selects only the actual
`/usr/bin/Hyprland` executable, a non-root identity with matching real/effective
UID, and the exact `WDDM_BRIDGE_COMPOSITOR_SESSION=1` option. It requires native
sync and negotiates the existing application profile 14. Other executables and
an absent selection retain their original main function. Normal argc, argv and
environment are passed through; no user, PAM login, configuration file, command
line, signal handler or input route is replaced.

The hook submits actual compositor commits through the existing native-buffer
and EGL native-fence ABI. It caches GPU entry points, uses a 64-bit sequence, and
closes each duplicated fence and destroys its EGL sync on all paths. The buffer
FD remains owned by the compositor. Ordinary submissions have zero audit/END
flags: no CPU buffer mapping, audit readback, upload or finite final-frame command
is added. There is no new worker or idle-frame producer.

EAGAIN retries keep the same buffer and fence, with one absolute five-second
retry budget and one-millisecond waits. The underlying bridge transaction retains
its separate transport deadline; five seconds is not a total bound on a blocking
native call. The existing bridge checks source-copy retirement before returning
success. Sequence, native-buffer, EGL or transport failures latch and reject later
commits and request one SIGTERM after fence cleanup. If main returns, the failing
session overrides that return with status 125. Hyprland owns its normal shutdown
handler; actual GPU fault status and retirement still need tests.

Startup, failure and return diagnostics use a separate nonblocking file description
and one best-effort write. Full pipes cannot block rendering, inherited flags
remain unchanged, and existing regular-file records are preserved by append.
An inaccessible or full sink can drop a diagnostic; it does not change the
operation result. Real full-pipe and existing-log-prefix controls pass.

`apply_aquamarine_session.py` adds the output on Aquamarine's existing idle event
queue after backend readiness. It forwards only the `WDDM-1` output and propagates
callback failures instead of silently accepting them. Every other source file,
including input code, remains unchanged. The adapter checks the complete source
hash before writing, supports both existing pinned Headless.cpp variants, verifies
the complete selected hash, and permits exact reapplication.

The current component supports **one fixed output**, selected as `1920x1080`
(default) or `3840x2160` with `WDDM_BRIDGE_SESSION_SIZE`. Format and size changes
inside a session are rejected. Normal monitor configuration is retained and must
match the selected output. Dynamic display modes and multiple native outputs
remain future work.

## Verification and retained failure

The real preload entry runs against a CPU-only compositor double, with mocked
GPU/EGL functions and real memfd/eventfd descriptors:

- **32 controls** pass, including 10,000 successive handoffs at each output size,
  unchanged arguments containing spaces, unmodified main exit status, EAGAIN
  retry/deadline, descriptor zero, reentrant-call rejection and error cleanup.
- Separate account controls reject root with status 125 and complete the 10,000
  handoffs at 4K under UID 65534. These do not establish an Omarchy login.
- Real descriptor counts balance; sync descriptors close and EGL syncs destroy
  after successful and failed submissions. The fence and GPU operations are
  explicitly mocked; this is not GPU, display, throughput or sustained-session
  evidence.
- The adapter changes only `src/backend/Headless.cpp` in the 79-file source tree
  derived from pinned Aquamarine commit `f31c47a1b9d300847d8fc3108ad959448103dfc1`
  and the published graphics/input patches. The complete inverse and exact
  reapplication pass, along with eight negative source/version controls.
- Both production and test-path hook libraries compile with C11, optimization,
  `-Wall -Wextra -Werror`. The complete public Aquamarine library builds with its
  existing project warning flags and local pinned dependencies. An earlier
  isolated build compiles but fails linking because its command omits the local
  libhyprutils search directory. That failed build is retained; a fresh public
  source build includes the required directory and links successfully.

CPU controls can be reproduced without a GPU, VM or desktop:

```sh
python3 experimental/windows-native-gpu/test_hyprland_session.py --output /tmp/new-hyprland-session-controls
```

The output directory must be fresh. Production compilation is:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC experimental/windows-native-gpu/hyprland_session.c -ldl -o /tmp/hyprland-session.so
```

These commands do not install or activate the backend. CI includes both CPU
control families; green CI does not establish desktop readiness.

## Remaining integration

The released launcher is unchanged. This hook is not yet installed by a normal
owner-aware graphics service or connected to official provisioning → SDDM →
`omarchy.desktop` → UWSM. The old root service and compositor fixture retain their
diagnostic limits. The persistent component still needs paired host/broker
activation, actual normal-session GPU frames, lifecycle/interrupt recovery and
long browsing/application sessions with bounded resources. Official Windows
keyboard and mouse responsiveness remain unverified. No new VM or desktop trial,
physical input, desktop switch or desktop automation is used for this change.

The requested **93% matched-native performance**, general Vulkan windows, CUDA,
video acceleration, DLSS and hardware ray tracing remain unverified. No readiness
notification or progress-gate promotion follows from these CPU/build results.

[Structured source/build/control evidence](evidence/persistent-compositor-source-2026-11-01.json).
[Earlier bounded 4K hardware evidence and timing limits](ROOT-BROKER-SESSION.md).
