# Host display incident: hardware testing on hold

**November 3, 2026:** the user reports extreme brightness and graphics corruption
across the physical Windows display. A follow-up confirms the problem persists.
**This is a release blocker. The backend is not ready for daily use.** Recovery
has not been confirmed, and further graphics/VM testing on this host is on hold.
Development continues through source work and verified CPU-only checks.

Read-only inspection records four NVIDIA `nvlddmkm` event 153 entries between
16:33:48 and 16:33:52 UTC. Their event data reports microcode reset, resetting,
reset and restarting TDR operations. Raw Windows event records remain private.
At 16:35:37 UTC no QEMU, native bridge or inactive-desktop test runner is found.
The preceding owned GPU run ended by 16:10:04 UTC. These times establish the
observations; they do not establish or exclude a causal link to that test.

The inactive desktop isolates windows and input, **not the physical GPU or its
display driver**. Unchanged driver files, a closed private desktop and zero
remaining owned processes do not prove that the driver/display is healthy.
Run 10's device-health queries also predate the reported host incident and
cannot establish the later health of the host display.

No display settings, HDR/gamma configuration, driver installation or reset is
performed in response. No further graphics workload is launched. The user is
given Microsoft's manual recovery steps; an actual reset/restart and successful
recovery have not been verified.

The [run 10 evidence](NORMAL-SESSION-CRASH-DIAGNOSTIC-2026-11-03.md) remains useful
for the draw-time Quickshell failure and incomplete VGA return. It does not
establish host driver stability. A future hardware validation plan must account
for the shared-driver failure boundary and the user's restriction on affecting
the host display. A separate test machine would avoid exposing this user's
active display, but none is currently provisioned or verified for this project.

Stable acceptance additionally requires investigation of the host fault,
confirmed recovery, and sustained end-to-end sessions without host display
corruption or driver resets. Existing launcher, official input, browsing,
multiple-window/4K, resource, cleanup and performance gates remain open.
Counts stay **backend 4/8, desktop 0/4, diagnostic scene 3/3**.

[Sanitized incident observations](evidence/host-display-incident-2026-11-03.json) ·
[Daily-use acceptance](DAILY-USE-READINESS.md).

[Source changes and offline controls](OFFLINE-SESSION-CONTROLS.md).
