# Release readiness review — September 22, 2026

## Current decision

The latest public launcher is `v0.0.20-preview`. The `v1.0.0` release is a
private draft and its publish workflow was cancelled before publication. The
source tree contains a newer signed candidate, but the public pin remains on
the accessible preview. No non-preview version has been approved or published.

Dropping `-preview` need not mean claiming 1.0 completeness. `v0.1.0` is a
possible first normal version after the gates below are met and a release
decision is made. Version numbers in this document are proposals, not a release
instruction.

## Evidence already in hand

- The [signed candidate record](evidence/V1-SIGNED-CANDIDATE-2026-09-22.md)
  covers an AMD Windows 11 laptop: authenticated payload download, GPU desktop,
  audio playback/capture devices, shared folder, Vulkan video playback, reboot,
  poweroff, repeat launch, update from the public launcher, interrupted update
  rollback, and preservation of existing guest data. The original installation
  was not changed for these tests.
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

The Windows About window was checked on the AMD laptop from an isolated
unsigned source build. The version, website, source/support address, notices
action, and all buttons were visible in the interactive desktop. This `-about`
check did not start the VM or change the installed guest. The app retains its
legacy taskbar application ID so existing pinned shortcuts keep grouping with
the branded window. A final signed release still needs its own visual and
installer acceptance; this check does not replace that gate.

## Gates for a first normal 0.x release

1. Keep the release story accurate across the executable, README, changelog,
   website, compatibility guide, and release notes. State supported Windows
   versions, x86_64 requirement, tested hardware, the CPU fallback, and known
   graphics limitations. Do not imply that the private draft is downloadable.
2. Validate the exact signed candidate on clean install and upgrade paths,
   including first boot, normal launch, files and settings preserved, input,
   audio, clipboard, sharing, camera, backup/restore, uninstall, and update
   recovery. Record the app, guest, runtime, Windows, and driver versions.
3. Resolve or clearly scope the Intel/NVIDIA Vulkan failure. A working CPU
   fallback is acceptable for a 0.x release only if it is automatic, usable,
   and accurately described. Confirm it on that hardware when available or
   seek an equivalent independent Windows test machine. Do not convert the
   unavailable PC into a passing result.
4. Exercise a Windows 10 host and at least one non-AMD Windows 11 host for the
   basic path, or explicitly narrow the first normal release's supported
   hardware and Windows-version claim. Check high DPI, fullscreen, ordinary
   keyboard/touchpad behavior, and sleep/resume on representative hosts.
5. Recheck signed update feeds and the preview-to-normal-version bridge before
   promotion. Publish only after the draft asset hashes, Authenticode signature,
   public download, rollback, and latest-release pointer have been checked.

## 1.0 quality bar

Treat 1.0 as the everyday Windows experience on the supported matrix: clear
installation and uninstall, a reliable desktop with honest graphics fallback,
no data loss through update or recovery, normal input and media, discoverable
Settings, accurate help and branding, and tested hardware beyond the single
AMD laptop. The gates above are the minimum evidence for that claim; 1.0 also
needs resolved high-impact reports from a broader user cohort.

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
