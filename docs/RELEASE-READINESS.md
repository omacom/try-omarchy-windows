# Release readiness review — September 23, 2026

## Current decision

The latest public launcher is `v0.0.20-preview`. The `v0.1.0` private draft
has passed guest build, headless boot smoke, asset verification, signing, and
physical Windows acceptance on the available AMD laptop. Its pinned source is
commit `9ae72c386f3d7aebc353b2924112c35e77a4475c`. The
[signed candidate record](evidence/V0.1.0-SIGNED-CANDIDATE-2026-09-23.md)
documents the exact test binary, upgrade, clean boot, recovery, backup,
restore, and uninstall. No normal version has been approved or published.

Dropping `-preview` need not mean claiming 1.0 completeness. `v0.1.0` is the
current normal-version candidate, subject to the gates below and a release
decision. A private draft is not a release instruction.

## Evidence already in hand

- The [signed candidate record](evidence/V1-SIGNED-CANDIDATE-2026-09-22.md)
  covers an AMD Windows 11 laptop: authenticated payload download, GPU desktop,
  audio playback/capture devices, shared folder, Vulkan video playback, reboot,
  poweroff, repeat launch, update from the public launcher, interrupted update
  rollback, and preservation of existing guest data. The original installation
  was not changed for these tests.
- The [signed clean-install record](evidence/V1-CLEAN-ACCEPTANCE-2026-09-22.md)
  covers first boot, a visible GPU desktop, a full backup and separate restore,
  exact guest-file preservation, an explicit CPU boot, a controlled automatic
  GPU-to-CPU fallback, and app uninstall of only the restored test copy. The
  original laptop installation and the clean test backup remain intact.
- The [r18 touchpad record](evidence/PINCH-R18-PHYSICAL-2026-09-22.md) includes
  a physical two-finger pinch. Initiation improved, zoom returned to normal,
  and scrolling still worked. The feature remains experimental and opt-in.
- The [runtime validation record](RUNTIME-VALIDATION.md) distinguishes the AMD
  pass from the earlier Intel/NVIDIA preview-runtime result. On that earlier
  configuration, OpenGL worked but Vulkan initialization failed. The Intel/
  NVIDIA PC is currently unavailable for Windows testing.
- The current source uses the official Omarchy logo in its icon, names the
  Windows app Try Omarchy, and identifies the publisher as Omacom. The
  [brand source](https://omarchy.org/brand/) reserves Omarchy trademark rights;
  brand presentation and the relationship stated on the site need a deliberate
  review before making a 1.0 claim.

## Brand and product experience pass

The live [Try Omarchy site](https://tryomarchy.com/) now uses the same Omarchy
mark as the Windows app, the same green accent, and an explicit link to the
current public `v0.0.20-preview` download. Its setup and uninstall descriptions
match the app's actual behavior; the site no longer promises that uninstalling
means deleting a single folder or that the next normal release must be 1.0.
Desktop and phone-width Chromium renders were inspected. The live `/download`
redirect was verified to resolve to the published preview launcher.
The site's macOS link and the README now point to
`https://github.com/omacom/try-omarchy`; the live support answer points to the
built-in diagnostics and GitHub bug-report flow.

The Windows About window was checked on the AMD laptop from an isolated
unsigned source build. The version, website, source/support address, notices
action, and all buttons were visible in the interactive desktop. This `-about`
check did not start the VM or change the installed guest. The app retains its
legacy taskbar application ID so existing pinned shortcuts keep grouping with
the branded window. A final signed release still needs its own visual and
installer acceptance; this check does not replace that gate.

The clean install revealed a shortcut collision between separate installations.
The signed test copy's Start Menu links were repaired to the original target.
The source now preserves links owned by another installation and gives
command-line restores folder-local launchers (PRs
[#154](https://github.com/omacom/try-omarchy-windows/pull/154) and
[#156](https://github.com/omacom/try-omarchy-windows/pull/156)). These fixes
postdate the old signed draft. The first signed `v0.1.0` candidate preserved the
existing links during a clean install, but offered the same unavailable Start
Menu choice again on the next boot. [PR #162](https://github.com/omacom/try-omarchy-windows/pull/162)
placed launchers beside a second installation and recorded the choice. The
re-signed binary passed both the first-run collision and repeat-launch checks
on the laptop.

## Gates for a first normal 0.x release

1. Keep the release story accurate across the executable, README, changelog,
   website, compatibility guide, and release notes. State the x86_64 and
   virtualization requirements, the CPU fallback, and known graphics
   limitations. Keep test-matrix detail in the compatibility and evidence docs.
   Do not imply that the private draft is downloadable.
2. Validate the exact signed candidate on clean install and upgrade paths,
   including first boot, normal launch, files and settings preserved, input,
   audio, clipboard, sharing, camera, backup/restore, uninstall, and update
   recovery. Record the app, guest, runtime, Windows, and driver versions.
3. Confirm the selected runtime's GPU path and automatic, usable CPU fallback
   on available physical Windows hardware. Keep the earlier NVIDIA Vulkan
   result in the compatibility guide and collect fresh reports when that
   hardware is available. Testing every CPU, GPU, or Windows version is an
   ongoing compatibility effort, not a release gate.
4. Recheck signed update feeds and the preview-to-normal-version bridge before
   promotion. Publish only after the draft asset hashes, Authenticode signature,
   public download, rollback, and latest-release pointer have been checked.

The `v0.1.0` candidate passed the available physical tests, including visual
inspection, GPU and CPU boots, core integrations, update rollback, backup,
restore, and uninstall. The original installation and shortcuts were preserved.
The remaining gate is a publication decision followed by the publish workflow's
public asset and feed checks, a physical preview-bridge-to-`v0.1.0` update, and
switching the prepared site PR to the newly public download. The public
download and update feed still point to `v0.0.20-preview`. Broader hardware
and Windows 10 coverage remain post-release compatibility work.

## 1.0 quality bar

Treat 1.0 as the everyday Windows experience on supported PCs: clear
installation and uninstall, a reliable desktop with honest graphics fallback,
no data loss through update or recovery, normal input and media, discoverable
Settings, and accurate help and branding. The gates above are the minimum
evidence for that claim. Collect hardware reports after release and fix
reproducible issues as they arrive; broader hardware coverage is not a gate.

Feature parity is tracked in [MAC-PARITY.md](MAC-PARITY.md). Live audio switching,
Windows Hello sudo, bridged networking, battery mirroring, and live RAM
reclamation are valuable, but none is automatically a 1.0 blocker if the
existing startup audio selection, password authentication, NAT networking,
power settings, and fixed memory allocation are reliable and their limits are
explained. Pinch should stay opt-in until broader device testing supports a
default. ARM64 and interface translation are open requests
([#131](https://github.com/omacom/try-omarchy-windows/issues/131),
[#127](https://github.com/omacom/try-omarchy-windows/issues/127)); neither can be
claimed as supported. Prioritize any gap that prevents a normal supported PC
from installing, using, updating, or removing the app over feature parity for
its own sake.
