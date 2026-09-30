# Windows and Mac feature review

Reviewed September 21 and refreshed September 30, 2026 against Mac source commit
[`e1a0dbe9820a8f7554ac3b930c2f13e1f41b25a1`](https://github.com/omacom/try-omarchy/tree/e1a0dbe9820a8f7554ac3b930c2f13e1f41b25a1).
This is an implementation and acceptance tracker, not a claim that every feature
is shipped or hardware-tested. The release gates in
[RELEASING.md](RELEASING.md) and [TESTING.md](TESTING.md) still apply.

The refreshed Mac baseline is newer than the original comparison.
It adds automatic startup with in-guest settings access, host battery mirroring,
guest-memory reclamation, precise trackpad scrolling, stable bridged identities,
keyboard-geometry and language work, update discovery, and runtime reliability
fixes. September source also adds experimental host USB passthrough, graphics
and audio continuity fixes, and safer management placement. The latest source
also follows Mac time-zone changes while running and avoids 9p writeback caching
for existing shared folders. Mac source features
are separate from its published `v0.4.1` release. Equivalent behavior is tracked below only where it makes sense on Windows.

## Corrections to the previous handoff

- Windows already has native first-run controls and five native Settings pages.
  The missing piece was an ordinary pre-boot entry point, not an entirely new UI
  framework. This candidate opens those controls before boot with a **Launch
  Omarchy** action. Explicit runtime commands retain direct startup; `-start`
  skips the launcher and `-launcher` explicitly opens it.
- A remote physical Windows test setup exists; use
  [REMOTE-LAPTOP-TESTING.md](REMOTE-LAPTOP-TESTING.md). The laptop must be online
  and signed in. Cross-compilation alone does not validate native windows.
- WHPX requesting nesting does not prove working guest KVM. Use the executable
  probe in [NESTED-VIRTUALIZATION.md](NESTED-VIRTUALIZATION.md).
- Mac and Windows pinch use a dedicated virtual multitouch touchpad. Windows
  ships gesture cancellation on focus loss and VM state changes with r18;
  broader application and hardware acceptance remains open.
- Windows endpoint selection ships through the r20c SDL backend. A hypothetical
  `-audiodev wasapi` switch is not an implemented backend in this runtime;
  follow the shipped SDL route rather than assuming such a switch exists.

## Current coverage

| Area | Windows status | Acceptance or implementation remaining |
| --- | --- | --- |
| Pre-boot launcher | Native pages and save-and-launch; signed candidate GPU boot/reboot/shutdown and keyboard regression tests pass | Mixed-DPI and broader hardware, moved-installation acceptance |
| Fullscreen monitor target | Settings choice and `-fullscreen-display` merged in #170; Windows native monitor enumeration and settings persistence passed | Second active monitor placement was not directly observed on the available laptop |
| Automatic startup | Owned Windows shortcuts can opt into direct startup while the Settings shortcut remains available | Broader physical acceptance |
| In-guest host settings | Shipped in `v0.2.0`; the signed candidate opened native Settings above the running VM on the AMD laptop | Further physical observations as reports arrive |
| Approved Windows apps | Phase 1 ships in `v0.2.0`; the signed candidate launched and revoked Notepad from Omarchy | [Embedded-window research #160](https://github.com/omacom/try-omarchy-windows/issues/160) and per-app icons |
| Branding and About | Omacom resource metadata, retained original copyright plus contributor credit, notices, labelled About actions; native visibility tested | Confirm public-facing relationship and presentation before a 1.0 claim |
| Camera, clipboard, shared folders, transfers | Implemented; the signed candidate passed camera and share checks on the AMD laptop. Since `v0.6.0` a file dropped from File Explorer reaches the app under the pointer ([#174](https://github.com/omacom/try-omarchy-windows/issues/174)) | Investigate concrete device reports |
| Resources, updates, storage and recovery | Implemented; the published update, backup, restore and uninstall paths passed on the AMD laptop | Broader hardware and recovery reports remain useful |
| GPU application compatibility | AMD GPU desktop and applications passed their recorded checks; a previous Intel/NVIDIA preview runtime booted VirGL OpenGL but failed Venus Vulkan and Godot Forward+ | [Current-runtime investigation #173](https://github.com/omacom/try-omarchy-windows/issues/173); retain CPU/OpenGL fallback |
| Nested KVM | Normal-user vCPU probe plus diskless Linux kernel/PID 1 boot, poweroff and reboot pass on the AMD laptop | Full nested distribution/storage/network workloads and wider host coverage; unsupported hosts must still boot Omarchy |
| Audio endpoint selection | Live host Settings and guest PipeWire switching ship in `v0.3.0` with r20c; public update and physical acceptance are in the [signed and public acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md) | [Live audio #167](https://github.com/omacom/try-omarchy-windows/issues/167): two physical endpoints per direction and hotplug need suitable hardware; see [audio behavior](AUDIO-DEVICES.md) |
| Trackpad pinch | [r18 bridge](PINCH-ZOOM.md), virtual touchpad and guest rules for new and existing guests ([#184](https://github.com/omacom/try-omarchy-windows/pull/184)); on by default for guest images that declare the device; synthetic and AMD-laptop physical Chromium pinch/scroll tests pass | Shipped in `v0.4.0`; Firefox and broader host/DPI/fullscreen acceptance |
| Windows Hello sudo | Opt-in since `v0.5.0`: launcher WebAuthn bridge, guest broker and a single PAM rule; one Hello prompt per sudo with password fallback ([design](WINDOWS-HELLO.md), [laptop run](evidence/HELLO-SUDO-LAPTOP-2026-09-26.md)) | Other Hello hardware (fingerprint, face) and Windows 10 |
| 1Password host authentication | Opt-in since `v0.6.0`: 1Password's system authentication unlock asks for Windows Hello through a polkit agent scoped to the installed 1Password process ([#176](https://github.com/omacom/try-omarchy-windows/issues/176)); canceling falls back to the guest password | 1Password still asks for its account password after it restarts |
| Bridged networking | NAT and explicit port forwarding exist | [True LAN bridge #166](https://github.com/omacom/try-omarchy-windows/issues/166), with supported adapter, privilege and firewall handling |
| Host battery | Shipped in `v0.2.0`; the AMD laptop's 99% charging state appeared as BAT0/ADP0 and in UPower | Desktop/no-battery transition remains to be observed on a suitable host |
| Guest RAM reclamation | Shipped with r19 in `v0.2.0`; three physical touch/free cycles returned about 797 MiB after the third 768 MiB allocation | Follow up on concrete memory reports |
| Keyboard and language | Windows keyboard layout and display language are read at launch. A candidate live time-zone channel preserves manual guest choices and offers Follow Windows Time Zone | Live following and the personal setup host-zone default require the new guest payload; public `v0.6.2` still reads the zone only at launch; physical ANSI/ISO/JIS geometry and broader input-method acceptance remain open |

Public `v0.6.2` is the current Windows release. Windows Hello sudo shipped in
`v0.5.0`; process-scoped 1Password unlock and direct application drops shipped in
`v0.6.0`. Live audio switching shipped in `v0.3.0`, and pinch is enabled by
default since `v0.4.0`. These are shipped features with the hardware limits
listed above.

True LAN bridging (#166) and embedded Windows app windows (#160) remain separate
work. The guest watchdog change in `v0.6.1` addresses service restarts during a
host suspension, but does not establish that #216's XWayland authorization
failure is resolved. Reporter confirmation and a physical Modern Standby S0
check remain outstanding.

Merged PRs [#235](https://github.com/omacom/try-omarchy-windows/pull/235),
[#236](https://github.com/omacom/try-omarchy-windows/pull/236) and
[#237](https://github.com/omacom/try-omarchy-windows/pull/237) share Mac's Tokyo
Night palette across the Windows launcher, Settings, About, USB and setup
windows. The launcher, Settings and setup also share the Try Omarchy mark and
monospaced heading. About and USB retain their native dialog structure.
Settings retain visible actions while pages scroll, hide manual CPU/RAM fields
for automatic profiles and explain when choices apply. This polish is merged,
but the published `v0.6.2` launcher predates it.
The visual comparison uses current Mac source and its public release capture;
a current native Mac run has not been observed. Native VM checks do not establish
mixed-monitor DPI, screen-reader user acceptance or broad physical coverage.

## Work sequence toward comparable everyday use

1. **Continue live audio hardware acceptance (#167).** [PR #175](https://github.com/omacom/try-omarchy-windows/pull/175)
   and the public `runtime-v1-r20c` ship live host Settings and guest PipeWire
   switching in `v0.3.0`; selected routes persist across guest reboots. The
   [public and physical acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md)
   documents the available laptop checks. Two physical endpoints per direction
   and hotplug remain to be tested on suitable hardware.
2. **Maintain shipped Windows Hello approval (#165).** Mirror the Mac's opt-in sudo
   model: enroll only after the guest password, pair a per-guest public key,
   sign a fresh request with Windows Hello, and verify it inside guest PAM.
   Denial and unsupported hosts must fall back to password. Approval, denial and password fallback were checked on the laptop.
   Shipped in `v0.5.0`. The Mac's separate, process-scoped 1Password unlock
   ([#176](https://github.com/omacom/try-omarchy-windows/issues/176)) followed
   in `v0.6.0` without changing general guest PAM policy.
3. **Offer a real LAN mode (#166).** Keep NAT and explicit forwards as the
   default. Start with a signed TAP adapter and a reversible wired-Ethernet
   bridge that has its own stable guest MAC. Verify host connectivity, guest
   DHCP/LAN reachability, restart, adapter loss and cleanup on a disposable
   wired setup. Only offer Wi-Fi bridging after an actual Wi-Fi proof.
4. **Finish host-app and file workflows (#160, #174).** The approved-app launch
   bridge already works. Window embedding needs capture, input, focus,
   accessibility, scaling and lifecycle behavior, and can ship once it is
   reliable in normal use. Direct drops shipped in `v0.6.0` through a guest
   drag source under the pointer.
5. **Polish input, language and graphics.** Use specific reports and available
   machines to address keyboard geometry/IME, shipped automatic pinch, and
   the Intel/NVIDIA Vulkan issue (#173).
   Keep the existing CPU/OpenGL fallback.

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

The original pre-boot launcher work is included in the public `v0.1.0` release; see its
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
`Verified`. That was a pre-implementation probe. Windows Hello sudo subsequently
shipped as an opt-in feature in `v0.5.0`. The
[availability API](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.ui.userconsentverifier.checkavailabilityasync)
allows an implementation to retain password authentication on unsupported hosts;
the later approval and denial checks are recorded in the
[Hello laptop report](evidence/HELLO-SUDO-LAPTOP-2026-09-26.md).
