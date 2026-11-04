# Gallium uploader failure ownership

**November 4, 2026:** a pinned Mesa 26.0.8 patch corrects two ownership errors
when upload-buffer allocation or CPU mapping fails. The complete original and
patched uploader are compiled against CPU Gallium callbacks, without a graphics
runtime. The original faults reproduce and all ten patched cases pass.
Hardware testing remains [on hold](HOST-DISPLAY-INCIDENT-2026-11-03.md).

`u_upload_alloc_buffer` releases a newly created resource after mapping failure
but leaves `upload->buffer` pointing to it. A later destroy or replacement can
release that reference again. The patch clears the owner pointer after release.

During replacement, that function transfers the old upload-buffer reference to
the caller's `releasebuf` output. If the new allocation/map fails,
`u_upload_alloc` overwrites that output with null, losing the reference. The patch
preserves it. The pinned `u_upload_mgr.h` contract explicitly assigns release
responsibility to the caller whenever this output is non-null. Existing failure
signals, allocation limits, successful paths and wire contracts remain unchanged.

The callback fixture retains dead records as tombstones, so duplicate releases
are measured without dereferencing freed memory or intentionally crashing a
process. It tests fresh/replacement allocation and map failures with both
persistent and nonpersistent mappings, plus successful data copies and reference
cleanup. Across the ten original cases it observes four dangling owner pointers,
four duplicate releases and four leaked references. These observations overlap;
they are counts in a CPU model, not measured NVIDIA allocations. All are zero
in the patched cases, and four successful original/patched data copies agree.

The regression tool compiles every function in the actual pinned uploader file.
Only includes are replaced with the callback fixture. Library assertions are
disabled to keep the failure experiment noncrashing; explicit fixture checks and
result validation remain active. Strict C11 warning checks pass on Windows with
the intentional upstream unsigned `~0` sentinel warning suppressed only around
the upstream source. Portable CI additionally runs the same tool with `cc`.

The source patch tool verifies version, input/output hashes and the reviewed
diff before one atomic file replacement. Repeated application verifies without
rewriting. Wrong-version, modified-source and corrupted-patch tests all reject
without changing the target. The regression input tree remains unchanged.

Apply to a fresh pinned Mesa tree when preparing a new runtime:

```sh
python3 experimental/windows-native-gpu/mesa/apply_mesa_upload_failure.py /path/to/mesa-26.0.8
```

The source file does not overlap the existing NVIDIA/native-render or Dozen
patches. A full Mesa runtime build and hardware integration are still required.
No installed driver, running graphics runtime or VM image is modified here.

The run-10 Quickshell stack reaches a uniform `memcpy` following an upload
allocation. Its saved reports lack pointer values, so this experiment does not
establish that either ownership error caused that crash. The state tracker's
unchecked null upload pointer has a separate
[CPU-tested guard](MESA-CONSTBUF-FAILURE-GUARD.md). The draw-time
crash, VGA-return failure, host display incident, API coverage and performance
requirements remain open. Counts stay **backend 4/8, desktop 0/4, diagnostic
scene 3/3**; daily use is unverified.

[Structured CPU evidence](evidence/mesa-upload-failure-cpu-2026-11-04.json) ·
[Run 10 crash observations](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md) ·
[Daily-use gates](DAILY-USE-READINESS.md).
