# Try Omarchy Windows continuation — September 23, 2026

The public release is [`v0.1.0`](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.1.0), a normal version with signed launcher, guest and r18 runtime. Thousands of users and relatively few bug reports are a useful positive signal for the everyday experience. Do not use broad Windows 10 or hardware coverage as an automatic gate for feature work or the next 0.x release. Fix reproducible reports as they arrive.

## Current candidate

Merged [PR #164](https://github.com/omacom/try-omarchy-windows/pull/164) implements three gaps in source: opening native host Settings from the Omarchy launcher, a Windows battery/AC mirror exposed as BAT0/ADP0, and live WHPX RAM reclamation through WINQ-EMU r19. The AMD Windows 11 laptop passed guest Settings, a real 99% charging battery in UPower, three RAM allocation/free/reuse cycles with about 797 MiB returned after the third cycle, and a full GPU-path desktop boot on r19. Read the [exact physical record](evidence/FEATURE-GAPS-2026-09-23.md) before changing acceptance claims.

The matching r19 portable and source archives, plus `SHA256SUMS`, are published at [`runtime-v1-r19`](https://github.com/omacom/try-omarchy-windows/releases/tag/runtime-v1-r19), targeting the exact CI build commit `751a845`. Source now pins r19 for the next signed app candidate. The public `v0.1.0` app still bundles r18. Rebuild the guest and validate the exact signed candidate before claiming these features are shipped to users.

The existing `D:\TryOmarchy-Gaps-20260923` folder on the laptop retains the isolated guest/runtime candidate. The user's normal installation was not changed. The Intel/NVIDIA PC remains booted into Omarchy and must not be switched to Windows for acceptance.

## Remaining feature work

- [Windows Hello sudo #165](https://github.com/omacom/try-omarchy-windows/issues/165): the laptop reports `DeviceNotPresent`; preserve guest password authentication until a scoped, fail-closed host/PAM bridge can be verified on a Hello-capable Windows 11 host.
- [True LAN bridge #166](https://github.com/omacom/try-omarchy-windows/issues/166): NAT and LAN port forwarding work; the laptop has no TAP adapter and active Wi-Fi. Driver installation and host adapter reconfiguration require an explicit supported, reversible design.
- [Live audio switching #167](https://github.com/omacom/try-omarchy-windows/issues/167): stable startup endpoint selection works; the current QEMU SDL backend has no supported live routing command.
- [Approved Windows app bridge #160](https://github.com/omacom/try-omarchy-windows/issues/160): merged [PR #169](https://github.com/omacom/try-omarchy-windows/pull/169) implements phase 1 with a host allowlist, guest launcher entries and fullscreen return. A Notepad round trip and revocation passed on the AMD laptop; the full guest image built and passed its boot smoke. Read the [physical record](evidence/WINDOWS-APP-BRIDGE-2026-09-23.md). It is not in public `v0.1.0`; embedded windows remain a separate milestone.
- [Fullscreen monitor choice #168](https://github.com/omacom/try-omarchy-windows/issues/168): merged [PR #170](https://github.com/omacom/try-omarchy-windows/pull/170) adds a General page target for fullscreen, uses that monitor's native resolution and restores to primary when the target is absent. Native Windows monitor enumeration and settings persistence passed on the AMD laptop. A second active monitor was not available for direct placement acceptance.
- [ARM64 #131](https://github.com/omacom/try-omarchy-windows/issues/131), [interface translation #127](https://github.com/omacom/try-omarchy-windows/issues/127), and the documented Intel/NVIDIA Venus Vulkan failure are compatibility work. The AMD laptop is not evidence for another GPU vendor.

The [feature tracker](MAC-PARITY.md), [1.0 quality bar](RELEASE-READINESS.md), [runtime checklist](RUNTIME-VALIDATION.md), and [remote laptop instructions](REMOTE-LAPTOP-TESTING.md) are the current source of truth. Historical September 22 handoffs and physical evidence are retained under `docs/evidence/`.

For all GitHub mutations under `/home/bts/Projects`, invoke `/home/bts/.local/bin/gh`, verify `gh api user --jq .login` returns `btsouth`, and use the personal git identity. Do not touch historical `bts-cssi` authorship; the user explicitly chose to leave it alone.
