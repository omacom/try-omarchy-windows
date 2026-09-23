# Settings, battery, and live RAM candidate — September 23, 2026

This is an engineering candidate on [PR #164](https://github.com/omacom/try-omarchy-windows/pull/164), **not** the public `v0.1.0` payload. The host was an AMD Ryzen 5 5625U laptop running Windows 11 Business build 26200. Tests used an isolated copy of the older installation on its D: drive; the user's normal installation was not changed. The Intel/NVIDIA PC was left running Omarchy as requested.

## Exact payloads

- The [Runtime workflow run](https://github.com/omacom/try-omarchy-windows/actions/runs/35852652612) built source and portable WINQ-EMU r19 archives. `runtime-build/verify.py` passed on the downloaded artifact.
- Both archives and `SHA256SUMS` are attached to the draft `runtime-v1-r19` component release, targeting the workflow's exact source commit `751a845359c98f0d31af9d5deee9d3d31ac5497f`. The draft is not publicly downloadable.
- `winq-emu-alpha10-portable.zip` SHA-256: `b417f30f01df11af64de696f9d74a95bedc918d66c3756b3210288a133bbd945`.
- `winq-emu-alpha10-source.zip` SHA-256: `bb175babf877bf07ea3b17eea06a0eddbb3069800450d1587423368ca7b4739e`.
- The locally built guest `rootfs.ext4` SHA-256: `9b9ef27233c236ec7f0afc4140778bf8cfb58d741f1febe4ac70fe966a72d25e`. Its exact kernel was `7.2.6-arch2-1`; the battery module compiled through DKMS for that kernel. Local guest smoke, patch application and contract checks passed.

## Physical checks

1. **In-guest Settings:** The guest's `try-omarchy-host-settings` command sent a one-shot request over the existing agent port. A native Try Omarchy Settings window opened visibly on the Windows desktop. The request did not disconnect the persistent guest agent. The tray logged a failed foreground handoff (`The parameter is incorrect`); the window itself opened, so foreground activation is still a polish issue.
2. **Host battery:** An interactive Windows `GetSystemPowerStatus` call reported a present, charging battery at 99%. The agent sent this state to the guest. The guest exposed `BAT0/capacity=99`, `BAT0/status=Charging` and `ADP0/online=1`; UPower reported 99% charging. The guest's critical-power action is set to Ignore because the Windows host owns shutdown. The test booted with a QEMU snapshot of the existing disk and the newly built guest kernel/initramfs; shutdown discarded the snapshot. A no-battery host and a real unplug/replug transition have not been observed.
3. **Live RAM return:** The public r18 runtime was used as a negative control. With free-page reporting forced on, it logged repeated `MADVISE not available` errors and retained roughly 4.72 GB working set. With r19 and a 4 GiB guest, `virtio_balloon` loaded and QMP still reported `actual=4294967296`, so the guest's assigned capacity was not reduced. Three 768 MiB allocate/touch/free cycles completed; a separate 16 MiB sentinel survived all three. On the third cycle QEMU's Windows working set fell from 2,573,266,944 bytes to 1,776,406,528 bytes after free, a return of 796,860,416 bytes. Windows available memory rose correspondingly. There were no discard errors. The test guest shut down cleanly.
4. **Full GPU boot on r19:** The isolated candidate launcher booted the existing Omarchy guest with r19, 4 vCPUs and 4 GiB memory. The render probe recorded `result=gpu` with AMD Radeon driver `31.0.21921.1000`. The guest reached Hyprland on a 1366×697 virtio GPU display, and its kernel logged `+virgl +edid +resource_blob +host_visible`. The agent and clipboard connected; QEMU stderr contained only the existing `Ignoring request for interrupt vector 0` warning. Guest poweroff ended the launcher normally. This proves desktop boot on the GPU path, not every OpenGL/Vulkan application or another GPU vendor.

## Shipping boundary

`guest-build/runtime.lock.json` still pins public r18. PR #164 enables reporting only when the selected runtime's provenance contains patch 0015, so merging source alone cannot make the public build reclaim RAM. Publish the validated r19 portable **and source** archives at an immutable runtime tag, update both URL/hash entries in the guest runtime lock, then build and sign the next candidate. Include the new guest image so Settings and BAT0/ADP0 reach existing and fresh installations. Do not describe these three features as shipped until that release is public.

The user base already supplies meaningful ongoing compatibility evidence: thousands of users and few bug reports are a positive signal. Additional broad hardware or Windows 10 coverage is not a prerequisite for this candidate. Reproducible reports should drive follow-up fixes.
