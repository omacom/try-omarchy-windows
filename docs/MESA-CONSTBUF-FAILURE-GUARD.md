# Constant-buffer upload failure guard

**November 4, 2026:** a source patch for pinned Mesa 26.0.8 stops the state
tracker from copying constants through a null upload pointer. It follows the
[uploader ownership fix](MESA-UPLOAD-FAILURE-LIFETIME.md). CPU callback tests
reproduce the original invalid-write paths and verify the guard. No graphics
runtime, VM, shader, driver or display operation is executed.

`st_upload_constants` previously continued after `u_upload_alloc` failed:
uniform copies and fixed-function state writes received the null pointer, then
an invalid constant buffer reached the binding callback. The patch checks the
pointer immediately after allocation. On failure it preserves deferred release
ownership, unbinds the stage's previous constant buffer, reports
`GL_OUT_OF_MEMORY`, and returns before those writes or a new binding. Other
stages' enable bits remain unchanged. Successful uploads retain the original
copy, binding and inlinable-constant logic.

The CPU regression compiles the complete pinned uploader with its ownership
fix, plus the complete original or patched `st_upload_constants` and
`st_unbind_unused_cb0` functions. Headers and their dependencies are represented
by CPU types/callbacks. A checked copy callback records a null destination
without dereferencing it; the state and bind callbacks do the same. Resource
records remain tombstones, allowing reference and duplicate-release checks
without a native crash. Library assertions are disabled; explicit fixture and
Python validation remain active.

Each original/patched variant runs 338 cases:

- 216 failures across all eight shader stages, uniform/state/mixed uploads,
  fresh and replacement resource creation/mapping, persistent and nonpersistent
  mapping, and nonpersistent remapping.
- 96 successful real-buffer/user-buffer cases checking actual copied bytes,
  256-byte alignment, state constants and inlinable values.
- 24 absent-program/absent-parameter/empty-parameter cases and two ATI local/global
  constant cases.

The original version reaches 144 invalid uniform copies, 144 invalid state
writes and 216 invalid bindings; these counts overlap. The guard prevents
those paths in all 216 failures and reports one error each. Every guarded case
then succeeds on retry after the simulated one-shot CPU failure. Deferred
references and final callback-model resource counts are checked throughout.
These retries do not establish GPU or application recovery.

The reviewed recipe verifies pinned version, both source hashes and patch
digest. It requires the matching uploader ownership fix before application.
Repeat application verifies without rewriting. Four negative controls reject
wrong version, a missing uploader fix, modified tracker source and a corrupted
patch without modifying the source. The regression only patches its own copies.
Strict MSVC C11 compilation and all 676 original/patched cases pass on Windows.
Upstream signedness warnings are suppressed only around upstream function text;
fixture code remains under warnings-as-errors. Portable CI runs the same test
with `cc`.

A separate real-header check compiles both complete affected C files, including
the state tracker's stage wrappers and UBO handlers, with Mesa's actual types
and declarations. The original and patched variants produce four MSVC object
files under a Windows D3D12 Meson configuration. This check uses upstream release
warning settings, not the callback fixture's warning profile. It reuses 34
captured generated headers from the pinned Linux build while generating Windows
compiler settings with Meson; Windows header generation is not independently
verified. The objects are neither linked nor loaded. This closes a source/header
compatibility gap without building or exercising the Linux guest runtime.
[Exact source/object hashes and limits](evidence/mesa-real-header-compile-2026-11-04.json).

Apply the two source patches to a fresh pinned tree in order:

```sh
python3 experimental/windows-native-gpu/mesa/apply_mesa_upload_failure.py /path/to/mesa-26.0.8
python3 experimental/windows-native-gpu/mesa/apply_mesa_constbuf_failure.py /path/to/mesa-26.0.8
```

A subsequent [full Linux-target D3D12/EGL/GBM build](MESA-LINUX-CROSS-COMPILE-2026-11-04.md)
compiles both patches and links six libraries without loading them. Neither
patch has been installed in the private runtime or a VM image. Linux packaging,
runtime and hardware integration remain unverified.
The void state-update function does not propagate a draw-cancellation result;
reporting an allocation error does not promise application-level recovery.

The [run-10 Quickshell stack](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md)
reaches a uniform copy after upload allocation, but its reports contain no
pointer values. This guard does not prove the cause or resolution of that
crash. Host display recovery, sustained normal sessions, cleanup, API coverage
and performance remain open. Hardware testing stays
[on hold](HOST-DISPLAY-INCIDENT-2026-11-03.md); daily use is unverified. Counts
remain **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

[Structured CPU evidence](evidence/mesa-constbuf-failure-cpu-2026-11-04.json).
