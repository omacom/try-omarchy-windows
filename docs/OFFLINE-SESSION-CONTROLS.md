# Offline scene and QMP controls

Following the [host display incident](HOST-DISPLAY-INCIDENT-2026-11-03.md), work
continues through source changes and CPU-only verification. No new VM, GPU
workload, driver operation, display setting change or desktop input is used.

The QMP readiness helper now reports a quiet connected peer's timeout as
`QMP control deadline exceeded`. An actual `select` failure retains its Winsock
error number. Existing deadlines, peer ownership checks, EOF handling and
fail-closed cleanup remain in place. Real Windows loopback-socket controls
exercise an expired deadline, a connected nonresponding peer, readable/writable
sockets, an invalid socket and orderly peer EOF. Both full strict Windows
build profiles pass. These checks do not initialize a graphics device.

The public `normal_scene_gate.py` accepts saved monitor, client and layer JSON
for the input-free single-output 3840×2160 diagnostic. It requires the exact
mapped Foot process, successful shell ping, background/panel layer candidates
and no unexpected Quickshell toplevels. Scale, metadata sizes and malformed
payloads are checked. The plain `quickshell` crash-reporter title cannot turn
a failed desktop into a passing scene.

Twelve synthetic unit tests pass. Offline replay also rejects all ten available
saved captures from runs 9 and 10, including the case with a mapped terminal and
successful IPC reply despite four crash reporters. No archived guest command
or report is executed. Raw saved metadata remains private.

Even valid structural candidates always report `visualInspectionRequired=true`,
`healthyDesktopAccepted=false`, `hostDriverStabilityAccepted=false` and
`dailyUseAccepted=false`. Layer geometry does not prove visible wallpaper,
correct pixels, input latency or a healthy host driver. Other workloads with
legitimate shell dialogs need their own policy. The new gate is exercised in
portable CI; the completed run 10 harness remains frozen and unchanged.

The Quickshell draw-time failure, stalled VGA return and host display incident
remain unresolved. Hardware testing on this computer remains on hold.

[Exact CPU control evidence](evidence/offline-session-controls-2026-11-03.json) ·
[Daily-use gates](DAILY-USE-READINESS.md).
