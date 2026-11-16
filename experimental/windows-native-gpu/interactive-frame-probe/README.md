# Continuous interactive compositor diagnostic

This is a bounded developer fixture, separate from the released launcher. It
keeps actual compositor handoffs flowing while application checks wait for user
input. The older fixture held the Windows picture at frame 1,799 until those
checks completed, although guest input and rendering continued.

`active_probe.c` and `multiwindow_control.h` are the project-owned controller
sources from guest build 208. `active_queue_probe.h` adds two background-tested
guards after that hardware trial: an end-of-stream acknowledgement is mandatory,
and authorization arriving during the cap frame cannot retroactively accept it.
The complete controller builds with strict warnings; these later guards have
mock queue coverage and have not received another interactive hardware run.

The controller requires at least 1,800 actual handoffs and all application,
resize, replacement and input checkpoints before requesting the final frame.
It allows up to 6,000 handoffs within the existing 180-second workload deadline.
Reaching the cap before the checks finish rejects the workload. Only the first
and actual final frame request an audit readback. Ordinary frames retain GPU-only
transport, fences and the existing ABI and byte limits. A final focus change
damages the owned browser after the checks complete. On failure, a one-pixel move
of the owned terminal supplies damage for the real drain acknowledgement while
preserving the original failure.

The companion browser protocol, owned RAM overlay and proprietary GPU runtime
are still required for guest integration. These sources alone are not a complete
guest image or a normal Omarchy session. QEMU SDL, keyboard, cursor and wheel
implementations are unchanged.

## Background verification

From the repository root on Linux/WSL:

```sh
python3 experimental/windows-native-gpu/test_interactive_frame_probe.py -v
```

This compiles the complete controller and exercises the actual queue hook with
mock EGL/native calls. It opens no windows, starts no VM, injects no input and
requires no GPU. Controls cover continuous handoffs beyond the old hold,
minimum/cap boundaries, a finish request during an in-flight cap frame, native
failure followed by a real final queue acknowledgement, missing entry points,
fence close/destruction errors and acknowledgement-write failure. These controls
do not prove GPU execution, ordinary input or performance.

The retained hardware trial and its failed input check are described in
[continuous input evidence](../../../docs/CONTINUOUS-INPUT-PROGRESS.md). The
auditor operates on saved files only:

```sh
python3 experimental/windows-native-gpu/interactive-frame-probe/audit_run249.py \
  /path/to/normal-gpu-run-249 /path/to/normal-gpu-v208/manifest.json \
  /path/to/new-audit.json
```

The full private artifacts are deliberately excluded from the repository. The
published evidence binds their hashes and preserves the rejected workload.
