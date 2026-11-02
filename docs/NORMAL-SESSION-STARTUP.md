# Normal-session startup boundary

**Root-service follow-up:** the server-side implementation now compiles and passes
CPU admission/lifecycle controls. Normal GPU boot and the actual systemd session
remain unverified. The historical client results below retain their scope.
[Root service and remaining packaging work](ROOT-SESSION-SERVICE.md).

**November 2, 2026:** the persistent hook now leaves Hyprland information queries
alone, and an unprivileged compositor exec wrapper builds and passes real Unix
credential/pidfd controls. A staged inner desktop entry retains the normal
watchdog. The production root activation service and normal GPU login are **not
implemented or verified** by these results. Counts remain **4/8, 0/4, 3/3**.

## Normal launcher contract

Read-only inspection of the installed guest confirms this outer entry:

```text
omarchy.desktop: uwsm start -g -1 -e -D Hyprland hyprland.desktop
hyprland.desktop: /usr/bin/start-hyprland
```

The inspected Hyprland source at commit
`1bd31aaa579a40680ce876e1547e371b60c45522` contains a
`Hyprland --version-json` probe in the watchdog's Nix detection. Its child setup
also adds `--watchdog-fd`, safe/locked flags, preserves the supplied compositor
arguments and executes the selected compositor path.

The earlier persistent hook would attempt native GPU negotiation for that version
query. A new regression against the exact published `e9a7023` hook reproduces
status 125 when the mocked GPU initializer fails, before the query reaches main.
This is a real entry-point regression; no failed SDDM boot is claimed.

`hyprland_session_invocation.h` recognizes the inspected non-rendering CLI paths
and leaves their original handling to Hyprland before GPU selection. It skips
option values correctly, including values literally named `--version-json`.
Unsupported or incomplete syntax is also left to the original argument parser.
Normal rendering retains the existing GPU/fence sequence and retirement checks.

`prepare_hyprland_session_entry.py` stages only this inner replacement:

```text
Exec=/usr/bin/start-hyprland --path /opt/omarchy-nvgpu/bin/hyprland-session-exec
```

It retains the `hyprland.desktop` ID and every other upstream byte. The outer
Omarchy entry, UWSM unit and Windows input code are untouched. Exact source hashes
and fresh-output checks prevent silent changes to a newer desktop entry or an
existing stage. This tool creates build-overlay files; it installs nothing.

## Compositor exec and activation

`hyprland_session_exec.c` executes the canonical Hyprland binary **without a fork**.
It preserves options, working directory, home, supplementary groups and inherited
watchdog descriptors; argv[0] names the canonical target. Query and syntax-only
invocations need neither runtime files nor an activation server.

For rendering, the wrapper requires an ordinary real/effective UID and GID with
no effective/permitted capabilities. It reads a bounded, root-owned regular
`/run/omarchy-nvgpu/compositor.env`: one link, no symlink or group/other write,
explicit version and matching UID/GID, literal quoted values, an allowlist of GPU
variables and no duplicate fields. It never imports HOME, PATH or arbitrary
variables. The file is a future root-service output; no owner is created or
hardcoded to UID 1000.

Before applying that environment, the wrapper connects to the dedicated Unix
seqpacket `/run/omarchy-nvgpu/session.sock`. It checks root UID/GID, pins the actual
root peer with `SO_PEERPIDFD`, sends a fresh random nonce and requires an exact
48-byte acknowledgement echoing it. Wrong fields, unsolicited/truncated packets
and ancillary descriptors are rejected; received descriptors close before the
operation returns. Socket I/O stays nonblocking with a single activation budget
of at most ten seconds, watching the held root peer throughout. A full runtime
lifetime is not limited to ten seconds.

After readiness, only the allowed graphics environment is applied. Legacy direct
Windows-worker/port and EGL-platform overrides are removed, then the same process
executes Hyprland. New control descriptors close before exec. The wrapper does
not change groups, session identity, signal handlers or the original watchdog's
parent-death policy. Actual root-side PID/cgroup admission and GPU startup remain
the responsibility of the **pending production root service**, not this client.

## Verification

- **49 preload controls** pass, including the earlier 32 controls, information
  queries, option-like literal values and a real inherited watchdog socket.
- **39 root-owned CPU/IPC controls** pass with a mocked GPU compositor and mock
  root activation service. A held peer pidfd follows the same PID from the
  wrapper into the exact compositor executable. Original home, supplementary
  groups and watchdog descriptors survive; 10,000 mocked 4K handoffs complete.
- Untrusted, malformed, duplicate, oversized, symlinked, hardlinked and writable
  runtime files fail **before any activation connection**. Root rendering is
  rejected; root and non-root information queries still work.
- Real socket controls reject altered messages, nonce mismatch, extra data,
  SCM_RIGHTS, control truncation, non-root peers and actual root-peer exit. File
  descriptor counts balance **inside the client before process exit**. The actual
  wrapper's ten-second deadline is separately exercised; no GPU work occurs.
- **Three staging controls** pass, including exact inversion to the installed
  upstream desktop file and preservation of an existing output directory.
- All C components compile with C11, optimization and `-Wall -Wextra -Werror`.
  CPU fixtures use real memfd/eventfd, sockets, credentials and process descriptors;
  every owned process is reaped and the isolated Linux temporary tree is removed.
  No account, host desktop, VM, renderer window or input is created or controlled.

These are source/build and CPU/IPC results. They do not prove actual watchdog
recovery, normal PAM/SDDM/UWSM operation, a persistent GPU frame or performance.
The older bounded GPU results retain their own scope.

## Next integration work

The root service must read the owner selected by official provisioning, prepare
this environment and activation socket, authenticate the actual wrapper via its
kernel pidfd/executable/cgroup, start the broker only when that wrapper arrives,
register the same process as primary and acknowledge readiness. It must preserve
existing active-worker/resource/byte limits and retire all GPU work before
cleanup or restart. Waiting for a user to finish provisioning must not consume
the broker's 30-second primary-registration deadline.

Boot visibility is a separate concrete gap: the installed owner provisioning
service uses tty1 before SDDM, while the current experimental graphics composer
replaces `virtio-vga-gl` with `-vga none`. The installed image's service invokes
`/usr/bin/omarchy-provision-owner`; the newer official builder adds an instant-trial
wrapper. Merely adding the instant kernel flag to this older image would not
install that newer flow. Normal first-boot console support and a correctly
prepared official factory overlay are still required; masking provisioning is
not a normal-session solution.

Normal GPU login, sustained browsing/apps through 4K, physical Windows input,
pressure and interruption recovery, bounded resources and the requested 93%
matched-native performance remain open. CUDA, general Vulkan windows, video,
DLSS and hardware ray tracing remain unverified. Daily-use readiness stays open.

[Structured evidence and exact source hashes](evidence/normal-session-startup-2026-11-02.json).
[Persistent compositor implementation and earlier scoped build](PERSISTENT-COMPOSITOR-SESSION.md).
