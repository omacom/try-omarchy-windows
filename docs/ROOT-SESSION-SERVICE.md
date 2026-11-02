# Root-owned session activation service

**November 2, 2026:** the root admission service and native-device preparation
tool are implemented and compile; an opt-in unit source is provided. The service passes
CPU/IPC controls using mocked kernel devices and a CPU broker. It has not booted
a normal GPU session. Counts remain **backend 4/8, desktop 0/4, scene 3/3**.

## Implemented startup and ownership

The service reads the root-owned SDDM autologin configuration and resolves its
selected account. It requires `Session=omarchy.desktop`, an ordinary UID/GID,
unique owner/session fields and trusted file metadata. It creates no user,
rewrites no login entry, and never launches a replacement PAM session.

Device preparation first requires the unique named `org.7wdev.wddm` guest port.
Only then does it load root-owned guest modules with the existing native-buffer,
sync, sparse CPU, coordinated-lease, desktop-fence/slot and multi-application
options. Existing modules are accepted only after capability and immutable
parameter checks. The service independently checks the prepared aperture and
render-node identity. Module loading and actual device discovery are **compiled,
not executed**, by the CPU checks.

The service publishes an owner-specific graphics environment and a bounded Unix
seqpacket listener, then notifies systemd that it can accept the compositor. It
starts no broker while waiting. A real wrapper must pass kernel peer credentials,
a held `SO_PEERPIDFD`, root-owned executable identity, all UID/GID fields, zero
permitted/effective capabilities and the exact configured UWSM cgroup.

After a complete valid nonce request, the service starts the existing coordinated
broker. The child receives only its explicit readiness and primary-identity
channels, with stop signals unblocked and the existing reference, live-retirement
and persistent-session modes selected. The service waits for broker readiness and
transfers the same live compositor pidfd before acknowledging the wrapper. The
wrapper then execs Hyprland in place. No graphics wire format, GPU budget,
active-worker limit or Windows keyboard/mouse code changes.

Each activation has an absolute deadline; idle service and desktop lifetimes have
no demonstration timer. One compositor lifetime is handled per service invocation.
Logout/relogin, watchdog restart and host-controller reuse still need integration
and real tests; this component does not establish those product behaviors.

## Shutdown and renderer faults

The service watches the compositor, broker and stop signal descriptors. Normal
compositor exit waits for broker retirement. Cancellation signals only the held
compositor identity, then waits for its broker. A failed startup, failed broker or
expired drain rejects success; owned broker processes are terminated and reaped.
The service removes its own runtime files with inode checks and never recursively
deletes runtime directories or unloads potentially live GPU mappings.

The unit is explicitly selected by `tryomarchy.nvgpu=1`, ordered after official
provisioning and before SDDM, with notify readiness and bounded start/stop handling.
It keeps the root service separate from the ordinary UWSM user session. Its
failure action requests guest poweroff to contain a failed native session. The
unit is a source artifact: it has not been installed, started under systemd or
exercised with GPU work. Device/VM fault containment remains unverified.

The persistent compositor hook also now requests **one SIGTERM after its owned
fence resources are released** on a fatal frame error. The inspected Hyprland
source already handles SIGTERM in its Wayland event loop. The hook adds no signal
handler and does not rely solely on a finite test main returning. CPU controls
verify resource-release order, one shutdown request and no request on healthy
frames. Real GPU error shutdown and retirement still require validation.

## Verification scope

- **33 service controls** plus one harness-cleanup control pass. A 31-second idle
  interval starts no broker and keeps descriptor count fixed, exceeding the
  broker's registration budget without consuming it.
- The real service and real wrapper hand off the same PID into the mock compositor
  and complete 10,000 mocked 4K handoffs. The broker's inherited root channel
  receives the live primary pidfd. The test cgroup is the harness's real kernel
  cgroup; the actual UWSM unit is not running in these checks.
- Wrong UID, executable and cgroup are rejected before broker startup. Malformed
  fields, zero nonce, short/long messages and descriptor injection/truncation
  leave the service alive with its original descriptor count and no broker.
- Broker startup failure, absent/bad readiness, post-registration failure and
  refusal to drain all fail closed and reap the owned broker. Idle, request-time
  and active cancellation pass. The real restrictive unit umask is exercised.
- Broker fixtures assert that only the intended descriptors cross exec and stop
  signals are unblocked. A process-scoped Linux subreaper independently confirms
  that no broker child is left orphaned. A deliberately orphaned CPU pipe holder
  verifies bounded harness failure cleanup and is itself reaped.
- Malformed, duplicate, root-account, untrusted-owner, writable and symlinked SDDM
  files fail before runtime paths are created. No account or user data changes.
- The final persistent hook retains **49 passing preload controls**, including
  native-fence cleanup and the new one-time renderer shutdown request. Production
  service, device-preparation and hook builds use C11, optimization and
  `-Wall -Wextra -Werror`.

GPU/EGL operations, guest device preparation and the broker's GPU implementation
are explicitly mocked in service execution. Actual credentials, sockets, pidfds,
exec, signals, process reaping and file metadata are real. These results prove
neither a normal GPU login nor performance or daily-use stability. No VM, module,
visible renderer, host input, desktop switch or desktop automation was used.

## Remaining product integration

The native runtime still needs a complete guest bundle with the exact kernel,
broker, shader/interop libraries, persistent hook and normal factory overlay.
Normal tty1 provisioning must remain visible; the current native argument composer
still removes VGA. The older installed image does not acquire the newer official
instant-trial wrapper merely from a kernel flag. The launcher must select the
matched persistent host mode and actual normal guest profile.

Normal application environment propagation, full shell/browser operation,
sustained sessions, pressure, interruption/recovery and physical Windows input
responsiveness remain required through 4K. The 93% matched-native performance,
CUDA, general Vulkan windows, video acceleration, DLSS and hardware ray tracing
requirements remain unverified. Daily-use readiness is still open.

[Structured controls and exact source/build hashes](evidence/root-session-service-2026-11-02.json).
[Client and original session contract](NORMAL-SESSION-STARTUP.md).
