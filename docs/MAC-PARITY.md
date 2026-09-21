# Windows and Mac feature review

Reviewed September 21, 2026 against Mac source commit
[`2be19b154f99e3b019c42b3b78f11c01f0ebe6a8`](https://github.com/omacom/try-omarchy/tree/2be19b154f99e3b019c42b3b78f11c01f0ebe6a8).
This is an implementation and acceptance tracker, not a claim that every feature
is shipped or hardware-tested. The stable release gates in
[V1-READINESS.md](V1-READINESS.md) still apply.

## Corrections to the previous handoff

- Windows already has native first-run controls and four native Settings pages.
  The missing piece was an ordinary pre-boot entry point, not an entirely new UI
  framework. This candidate opens those controls before boot with a **Launch
  Omarchy** action. Explicit runtime commands retain direct startup; `-start`
  skips the launcher and `-launcher` explicitly opens it.
- A remote physical Windows test setup exists; use
  [REMOTE-LAPTOP-TESTING.md](REMOTE-LAPTOP-TESTING.md). The laptop must be online
  and signed in. Cross-compilation alone does not validate native windows.
- WHPX requesting nesting does not prove working guest KVM. Use the executable
  probe in [NESTED-VIRTUALIZATION.md](NESTED-VIRTUALIZATION.md).
- The Mac pinch implementation is a dedicated virtual multitouch touchpad, not
  a Hyprland zoom shortcut. Windows parity needs equivalent event delivery,
  including cancellation on focus loss and VM state changes.
- The pinned QEMU SDL and DirectSound options do not expose endpoint selection.
  A hypothetical `-audiodev wasapi` switch is not an implemented backend in this
  runtime. SDL itself uses Windows audio APIs; endpoint selection could extend
  the existing SDL backend instead of requiring a wholesale backend replacement.

## Current coverage

| Area | Windows status | Acceptance or implementation remaining |
| --- | --- | --- |
| Pre-boot launcher | Native pages and save-and-launch; physical GPU boot/reboot/shutdown and keyboard regression tests pass | Mixed-DPI and broader hardware, moved-installation acceptance and final signed candidate |
| Branding and About | Omacom resource metadata, retained original copyright plus contributor credit, notices, labelled About actions; native visibility tested | URL actions and final signed candidate acceptance |
| Camera, clipboard, shared folders, transfers | Implemented, with existing physical evidence in the handoff | Retest the selected final candidate; device coverage remains bounded |
| Resources, updates, storage and recovery | Existing implementation; available before boot in this candidate | Existing v1 gates, plus recovery from the new launcher |
| Nested KVM | Normal-user vCPU probe plus diskless Linux kernel/PID 1 boot, poweroff and reboot pass on the AMD laptop | Full nested distribution/storage/network workloads and wider host coverage; unsupported hosts must still boot Omarchy |
| Audio endpoint selection | Startup playback/recording choices implemented for the r16 candidate; [behavior and acceptance](AUDIO-DEVICES.md) | Stable endpoint IDs, live guest selection bridge, and switching between two physical devices |
| Trackpad pinch | [Opt-in r17 bridge](PINCH-ZOOM.md), virtual touchpad and factory guest configuration implemented; fresh-image and synthetic Windows-to-Chromium zoom tests pass | Physical gestures, Firefox and broader host/DPI/fullscreen acceptance; experimental only |
| Windows Hello sudo | Not implemented; guest password authentication remains | Host Hello availability and verification, authenticated request bridge and fail-closed PAM integration; rejection, timeout, cancellation and unsupported-host tests |
| Bridged networking | NAT and explicit port forwarding exist | Supported adapter/driver implementation and distribution, privilege boundary, reconnect and firewall behavior; no silent installation or adapter reconfiguration |

Windows Hello, live audio routing and bridging remain feature work; pinch still
needs acceptance before default enablement. A
settings link, source-only runtime patch, or build success does not close those
rows. Keep native platform differences explicit instead of adding controls that
cannot deliver their labelled behavior.

## Local checks and next laptop pass

The candidate includes Windows-only native choice, hidden-startup, location and
launcher-keyboard regression tests. Run in an interactive desktop with
`TRYOMARCHY_UI_TEST=1` and `TRYOMARCHY_LAUNCHER_TEST_EXE` pointing at the candidate.
The [physical validation record](evidence/MAC-PARITY-2026-09-21.md) separates
completed checks from remaining acceptance. Compile the test binary with:

```sh
cd app
GOOS=windows GOARCH=amd64 go test -c -o /tmp/TryOmarchy-parity-tests.exe
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags '-H windowsgui -s -w' -o /tmp/TryOmarchy-parity.exe .
```

Use a fresh candidate filename and verify its checksum before execution. Do not
replace the accepted launcher or mutate a running guest disk.

1. Test choice controls and About without touching the installation.
2. Open `-dir <existing installation> -launcher`; inspect every page, Tab,
   Shift+Tab, Enter, Escape, and the reachable footer. Closing must not boot.
3. Exercise the labelled location dialog against an isolated new data directory.
   Cancellation must not start downloads. Exercise default and selected paths.
4. Start the existing guest from **Launch Omarchy**, verify persisted resources,
   shortcuts, file fixtures and clean guest shutdown. `-start` and scripted
   runtime arguments must bypass the menu. Existing `-settings` must only save.
5. Exercise recovery through the launcher on a disposable copy; it must not be
   blocked by a lifecycle listener owned by the idle launcher. Check that a move
   reopens the correct launcher and location.
6. Run the KVM probe inside the guest as its ordinary user and retain the JSON
   alongside exact runtime and host facts.

This engineering work is retained on `codex/laptop-control-handoff`. It has not
been merged into master or published as a release. See [the next-session handoff](NEXT-SESSION.md).

## September 21 host capability checks

The test laptop reports an ELAN PrecisionTouchpad Filter Driver. Its Windows 11
build exports the touchpad APIs, and registering a temporary test window with
`RegisterTouchpadCapableWindow` succeeded. This is an implementation lead, not a
working pinch bridge by itself. The subsequent [experimental implementation](PINCH-ZOOM.md)
has synthetic end-to-end evidence. The [Microsoft programming contract](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/registertouchpadcapable)
requires the window owner to handle resulting pointer messages and preserve
normal scrolling. Unsupported Windows versions retain ordinary input; this path
has not been tested on Windows 10. Actual finger gestures remain untested;
synthetic guest libinput delivery was tested in the continuation.

`UserConsentVerifier.CheckAvailabilityAsync` returned `DeviceNotPresent`, both
through OpenSSH and in an interactive scheduled task for the signed-in user.
No verification prompt or guest PAM change was made. The
[availability API](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.ui.userconsentverifier.checkavailabilityasync)
allows an implementation to retain password authentication on unsupported hosts;
this host cannot currently validate a successful Hello authentication flow.
