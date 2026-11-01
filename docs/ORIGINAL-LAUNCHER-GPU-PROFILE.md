# Original launcher argument profile

**Latest November 1 follow-up:** bounded run 30 passes 1,800 handoffs in
58.177667 seconds, exact first/final 4K pixels, authenticated native work for
both Chromium GPU lifetimes and Nautilus, and live cleanup. Tab retirement,
producer lifetime and coherent diagnostic-output fixes retain earlier failures.
Normal sessions, launcher activation, physical input, long sessions and daily
use remain open; counts stay **4/8, 0/4, 3/3**.
[Current desktop/shader captures and precise proof](ORIGINAL-PROFILE-APPLICATIONS.md).
The older observations below retain their original scope.

The native owner now has an explicit profile that consumes the original Go
launcher command. It replaces the graphics device through the existing composer
and adds private graphics/QMP transport while preserving the original boot,
disk, network, audio, cursor, keyboard/tablet and official QMP arguments.
The Go profile producer and native runtime consumer are implemented.

**October 31 follow-up:** a private RAM-overlay guest now boots through this
profile and produces an actual 4K NVIDIA source/SDL frame with complete pixels
matching exactly. Only three handoffs complete against the unchanged target of
1,800, so the full regression remains rejected. Released settings still leave
native activation unset. Normal account startup, end-to-end interaction and
daily use remain open. Counts stay **4/8, 0/4, 3/3**.
[Actual frame, named-device fixes and retained attempts](LAUNCHER-GUEST-GRAPHICS-BOOTSTRAP.md).

The first subsequent real VM attempts reach the new process binding on an
inactive private Windows desktop but fail before guest boot. The observed socket
reconnect crash matches an upstream QEMU fix, now backported into the experimental
runtime recipe. [Failure, isolation controls and remaining checks](QEMU-SOCKET-RECONNECT.md).

## Argument and ownership contract

The internal Go `nativeGPULauncherProfile.prepare` receives the original
`exec.Cmd`. Its bounded protocol-1 JSON carries QEMU's absolute executable path,
the original argument vector and an explicit audit directory. JSON is encoded
as ASCII with Unicode surrogate escapes, then decoded to UTF-16 inside the
native runtime. Paths with spaces, non-ASCII characters and an emoji pass the
cross-language checks. Strings with NUL/CR/LF, invalid UTF-8, excessive payloads
or excessive fully quoted Windows command length are rejected.

The owner uses the documented
[UTF-8/UTF-16 conversion](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)
and [CreateProcessW command-line bound](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw).
The original environment, directory and streams are retained at the existing
Go supervision boundary. An explicit empty environment remains distinct from
an inherited environment, including when a profile mutates its input command.

Native parsing checks exact schema/unique keys, size and nesting limits,
original boot options, QEMU/WHPX, memory bounds, a single display and the
existing graphics/input compatibility rules. CPU, Venus and multiple-display
profiles are rejected. The selected runtime uses the original executable and
arguments, retaining the machine's original branding for authenticated QMP
name checks. It appends two private loopback QMP roles while keeping all
original Unix control roles. The SDL options check retains the caller's
cursor preference.

The full native desktop permission/capacity agreement and inherited lifetime
control are required. Diagnostic input, read-only-root replacement, forced
EOF/store controls and extra diagnostic network devices cannot be combined
with the original-argument profile. Existing flag-absent guards retain their
earlier behavior. The explicit audit directory avoids changing the launcher's
input environment. QEMU inherits a duplicate of the original per-attempt log
file, so its startup errors still reach the launcher's existing classifiers.

## Early process binding and later control readiness

For this profile, after suspended creation, verified file/process adoption and
job assignment, the native owner emits `nativeLauncherVmBound` before resuming
QEMU. The record retains the exact process/creator creation times and keeps
`qmpReady=false` and all graphics/input/daily-use acceptance flags false.
Go validates and holds that process object, then publishes its actual PID to
the existing supervisor. An already-reaped process is not published as a live
PID.

The native constructor observes the original ten-second boot head start before
private QMP access, checking creator/cancellation liveness during that wait.
It later emits the existing `nativeLauncherVmReady` record. Go requires this
transition to identify the same held VM; it rejects duplicate bindings and
altered identities. Identity reads are synchronized across the transition.
The original launcher supervision and retry code in `main.go` is unchanged
from the preceding Go-supervisor checkpoint.

Early binding lets a VM exit before private control readiness be delivered
through the existing process-wait path. Hidden helper tests verify that this
exit reaches the original memory-error classifier. The test supplies a
synthetic memory-error line; it does not establish actual QEMU OOM recovery
or a completed normal boot/retry cycle.

## Verification, October 29, 2026

- Both strict default and 4K native builds pass **30 profile checks**, the
  existing **48 ownership checks** and **37 lifetime checks**.
- Eight selected Windows Go test methods and static checking pass, covering
  Unicode/quoting, nil/empty/value environment snapshots, protocol parsing,
  default child processes and the existing QEMU bridge/peer behavior.
- Six profiles generated by the actual `buildQemuArgs` function cross the real
  Go encoder/native parser: cursor off, cursor on and Unicode pass; CPU, Venus
  and multiple displays are rejected. Original non-graphics arguments are
  compared in full.
- Five real hidden helper-process controls cover binding before simulated
  readiness, exit before readiness, cancellation before readiness, an altered
  creation time and a duplicate binding. The helpers use the same native
  identity producer and Go consumer, with real held process/job ownership.
  Their QMP readiness is synthetic; they start no QEMU or GPU.
- Eight CLI rejection checks pass. The exact full flag vector produced by Go
  additionally reaches lifetime validation and rejects an invalid inherited
  token before runtime/VM creation.

Three separate hidden QEMU/WHPX guests regress the refactored binaries with
the new profile **disabled**. The original images, NVIDIA 621.01 snapshot,
kernel, memory guards, shaders and acceptance oracles are unchanged. Vulkan
pressure recovery passes both full 16,384-word rounds and retires all 13
submissions/20 signals; 4K compute passes both 8,294,400-word rounds; both
3840×2160 shader frames pass complete pixels and shared-texture handoff.
Shader bytes match the preceding checkpoint. Native objects and mappings
retire before natural VM exit. No visible window, host input, user trial or
guest source disk was used.

[Sanitized audit evidence](evidence/ORIGINAL-LAUNCHER-GPU-PROFILE-2026-10-29.json)
binds the current/frozen native compiler inputs, 434 Go/module files, binaries,
CPU contracts, helper transitions, prior raw regressions and complete GPU
data/pixel checks. Earlier pre-binding builds and regression observations are
retained separately. They do not count as final profile acceptance.

The background-safe argument/bootstrap follow-up above now reaches GPU work;
its incomplete workload still needs diagnosis. Normal account
startup, official Windows interaction, sustained browsing/multiple apps through
4K, resource-pressure and active-GPU recovery, broad API support and at least
93% matched-native performance remain required before daily-use readiness.
