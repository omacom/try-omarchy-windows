# Windows and Mac feature review

Reviewed September 21 and refreshed September 23, 2026 against Mac source commit
[`28f4722fab3e16ae26a7cb8fab2ab7908b1833e4`](https://github.com/omacom/try-omarchy/tree/28f4722fab3e16ae26a7cb8fab2ab7908b1833e4).
This is an implementation and acceptance tracker, not a claim that every feature
is shipped or hardware-tested. The release gates in
[RELEASING.md](RELEASING.md) and [TESTING.md](TESTING.md) still apply.

The refreshed Mac baseline is newer than the original comparison.
It adds automatic startup with in-guest settings access, host battery mirroring,
guest-memory reclamation, precise trackpad scrolling, stable bridged identities,
keyboard-geometry and language work, update discovery, and runtime reliability
fixes. Equivalent behavior is tracked below only where it makes sense on Windows.

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
| Pre-boot launcher | Native pages and save-and-launch; signed candidate GPU boot/reboot/shutdown and keyboard regression tests pass | Mixed-DPI and broader hardware, moved-installation acceptance |
| Fullscreen monitor target | Settings choice and `-fullscreen-display` merged in #170; Windows native monitor enumeration and settings persistence passed | Second active monitor placement was not directly observed on the available laptop |
| Automatic startup | Owned Windows shortcuts can opt into direct startup while the Settings shortcut remains available | Broader physical acceptance |
| In-guest host settings | Shipped in `v0.2.0`; the signed candidate opened native Settings above the running VM on the AMD laptop | Further physical observations as reports arrive |
| Approved Windows apps | Phase 1 ships in `v0.2.0`; the signed candidate launched and revoked Notepad from Omarchy | [Embedded-window research #160](https://github.com/omacom/try-omarchy-windows/issues/160) and per-app icons |
| Branding and About | Omacom resource metadata, retained original copyright plus contributor credit, notices, labelled About actions; native visibility tested | Confirm public-facing relationship and presentation before a 1.0 claim |
| Camera, clipboard, shared folders, transfers | Implemented; the signed candidate passed camera and share checks on the AMD laptop | [Direct application drops #174](https://github.com/omacom/try-omarchy-windows/issues/174); investigate concrete device reports |
| Resources, updates, storage and recovery | Implemented; the published update, backup, restore and uninstall paths passed on the AMD laptop | Broader hardware and recovery reports remain useful |
| GPU application compatibility | AMD GPU desktop and applications passed their recorded checks; a previous Intel/NVIDIA preview runtime booted VirGL OpenGL but failed Venus Vulkan and Godot Forward+ | [Current-runtime investigation #173](https://github.com/omacom/try-omarchy-windows/issues/173); retain CPU/OpenGL fallback |
| Nested KVM | Normal-user vCPU probe plus diskless Linux kernel/PID 1 boot, poweroff and reboot pass on the AMD laptop | Full nested distribution/storage/network workloads and wider host coverage; unsupported hosts must still boot Omarchy |
| Audio endpoint selection | Startup choices and stable endpoint IDs ship; merged r20c source, [signed packaged laptop checks](evidence/LIVE-AUDIO-R20-2026-09-23.md) and a disposable image upgrade establish live host routing, guest PipeWire choices, restart persistence and idle release; [behavior](AUDIO-DEVICES.md) | [Live switching #167](https://github.com/omacom/try-omarchy-windows/issues/167): pin release artifacts; two physical endpoints and hotplug need suitable hardware |
| Trackpad pinch | [Opt-in r18 bridge](PINCH-ZOOM.md), virtual touchpad and factory guest configuration implemented; synthetic and AMD-laptop physical Chromium pinch/scroll tests pass | Firefox and broader host/DPI/fullscreen acceptance; experimental only |
| Windows Hello sudo | Not implemented; guest password authentication remains | [Opt-in authentication bridge #165](https://github.com/omacom/try-omarchy-windows/issues/165); the signed-in laptop reports Hello `Available` and completed an initial approval prompt, but key-backed guest sudo remains untested |
| 1Password host authentication | Guest 1Password uses its ordinary password and Linux authentication paths | [Windows Hello unlock #176](https://github.com/omacom/try-omarchy-windows/issues/176) follows #165 with a narrowly scoped agent for the installed guest 1Password process and Mac-style process/polkit checks |
| Bridged networking | NAT and explicit port forwarding exist | [True LAN bridge #166](https://github.com/omacom/try-omarchy-windows/issues/166), with supported adapter, privilege and firewall handling |
| Host battery | Shipped in `v0.2.0`; the AMD laptop's 99% charging state appeared as BAT0/ADP0 and in UPower | Desktop/no-battery transition remains to be observed on a suitable host |
| Guest RAM reclamation | Shipped with r19 in `v0.2.0`; three physical touch/free cycles returned about 797 MiB after the third 768 MiB allocation | Follow up on concrete memory reports |
| Keyboard and language | Windows time zone, keyboard layout and display language follow the host | Physical ANSI/ISO/JIS geometry and broader input-method acceptance |

Windows Hello, true bridged networking, and embedded Windows app windows remain
feature work. Live audio source is merged but awaits a release pin. Settings,
battery mirroring, live memory
reclamation and approved app launch passed the
[signed v0.2.0 candidate](evidence/V020-SIGNED-CANDIDATE-2026-09-23.md).
Pinch remains opt-in; its physical
gesture and scrolling checks passed on the laptop.

## Work sequence toward comparable everyday use

1. **Complete live audio (#167).** [PR #175](https://github.com/omacom/try-omarchy-windows/pull/175)
   merged the r20c host routes and guest PipeWire picker. The signed candidate
   passed the [available laptop checks](evidence/LIVE-AUDIO-R20-2026-09-23.md).
   Pin the release artifacts after review. Two physical endpoints and hotplug
   need additional hardware; their absence does not hold up this laptop pass.
2. **Add signed Windows Hello approval (#165).** Mirror the Mac's opt-in sudo
   model: enroll only after the guest password, pair a per-guest public key,
   sign a fresh request with Windows Hello, and verify it inside guest PAM.
   Denial and unsupported hosts must fall back to password. After PIN setup,
   the current laptop can test approval and denial in the eventual guest flow.
   Once that bridge is sound, implement [#176](https://github.com/omacom/try-omarchy-windows/issues/176),
   the Mac's separate, process-scoped 1Password unlock integration, without
   changing general guest PAM policy.
3. **Offer a real LAN mode (#166).** Keep NAT and explicit forwards as the
   default. Start with a signed TAP adapter and a reversible wired-Ethernet
   bridge that has its own stable guest MAC. Verify host connectivity, guest
   DHCP/LAN reachability, restart, adapter loss and cleanup on a disposable
   wired setup. Only offer Wi-Fi bridging after an actual Wi-Fi proof.
4. **Finish host-app and file workflows (#160, #174).** The approved-app launch
   bridge already works. Window embedding needs capture, input, focus,
   accessibility, scaling and lifecycle behavior; direct drops need a Wayland
   target protocol rather than a Downloads fallback. Each can ship separately
   once it is reliable in normal use.
5. **Polish input, language and graphics.** Use specific reports and available
   machines to address keyboard geometry/IME, opt-in pinch defaults, and the
   Intel/NVIDIA Vulkan issue (#173). Keep the existing CPU/OpenGL fallback.

Public `v0.2.0` already covers the former battery, unused-RAM, fullscreen and
in-guest Settings gaps. Broad hardware or Windows-version coverage is ongoing
compatibility work. Each new path above needs proof of its own behavior before
we call it complete; unrelated user hardware is not a release gate.

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

This engineering work is included in the public `v0.1.0` release; see its
[signed acceptance and public update record](evidence/V0.1.0-SIGNED-CANDIDATE-2026-09-23.md).

## September 21 host capability checks

The test laptop reports an ELAN PrecisionTouchpad Filter Driver. Its Windows 11
build exports the touchpad APIs, and registering a temporary test window with
`RegisterTouchpadCapableWindow` succeeded. This is an implementation lead, not a
working pinch bridge by itself. The subsequent [experimental implementation](PINCH-ZOOM.md)
has synthetic end-to-end evidence. The [Microsoft programming contract](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/registertouchpadcapable)
requires the window owner to handle resulting pointer messages and preserve
normal scrolling. Unsupported Windows versions retain ordinary input; this path
has not been tested on Windows 10. Actual finger gestures passed on the AMD
laptop with r18: pinch was easier to start than before, zoom returned, and
two-finger scrolling still worked. See
[the physical test record](evidence/PINCH-R18-PHYSICAL-2026-09-22.md).

`UserConsentVerifier.CheckAvailabilityAsync` initially returned
`DeviceNotPresent`, both through OpenSSH and in an interactive scheduled task
for the signed-in user. After Windows Hello PIN setup on September 23, the
interactive task returned `Available`, and an initial approval prompt returned
`Verified`. No guest PAM change has been made. The
[availability API](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.ui.userconsentverifier.checkavailabilityasync)
allows an implementation to retain password authentication on unsupported hosts;
the laptop is now available for a key-backed guest approval and denial test.
