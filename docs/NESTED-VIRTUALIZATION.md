# Nested virtualization on Windows

Minimal nested KVM execution and a diskless Linux kernel/PID 1 boot are
**verified on one AMD Windows 11 laptop**.
This is not a general hardware-support guarantee. The graphics runtime asks
WHPX to enable nesting when the in-kernel interrupt controller is active and
Windows advertises `NestedVirtSupport`. A successful request, `-cpu host`, or
`vmx`/`svm` in `/proc/cpuinfo` does not by itself establish usable guest KVM.

The pinned runtime's patch `0001` lets the desktop boot if Windows refuses that
request. It logs `WHPX: nested virtualization unavailable` and continues without
nesting. The launcher's `kernel-irqchip=off` fallback avoids the request entirely.
CPU rendering uses a different CPU model; do not extrapolate a GPU-path result
to that fallback. None of these paths promises nested support on a host where
Windows does not provide it.

## Verify inside the guest

Run the repository's probe as the regular Omarchy user, inside the Windows-hosted
guest (copy it through a shared folder, or send it through existing guest SSH):

```sh
python3 check-nested-virtualization.py
```

The source is [scripts/guest/check-nested-virtualization.py](../scripts/guest/check-nested-virtualization.py).
It uses Python's standard library, opens `/dev/kvm`, checks API version 12,
creates a VM and vCPU, maps one page, and actually executes a real-mode `HLT`
instruction. It reports JSON and exits 0 only for `KVM_EXIT_HLT`. A ten-second
deadline bounds the probe. It creates no disk or network and changes no settings.

Run without sudo first: a root-only pass does not establish that normal-user
virtualization works. A failure reports the stage and error. Missing `/dev/kvm`
can mean missing kernel support/modules or no exposed virtualization extensions;
permission denied is a separate user-access problem. Do not switch off Windows
security features, change Hyper-V roles, or reset the guest as a troubleshooting
shortcut.

For each result, record Windows build, CPU, launcher SHA-256, runtime receipt,
guest kernel, GPU/CPU mode, actual QEMU arguments and the nested-related runtime
log. The same probe passing on the Linux development host proves only the
probe's operation. A pass inside Omarchy proves a minimal nested vCPU works on
that combination. The diskless Linux test below adds boot/reboot/shutdown
evidence; a full nested distribution and broader hosts still need acceptance
before advertising general nested virtualization.

## Current evidence

September 21, 2026: the probe passed both on the Linux development host and inside
the Windows-hosted Omarchy guest, including a repeat after guest reboot. The
physical host was a Ryzen 5 5625U with Radeon graphics, Windows 11 build 26200.
The guest ran kernel `7.2.6-arch2-1`, the public v20 runtime, GPU mode, `-cpu host`
and `q35,accel=whpx` without the irqchip fallback. The normal Omarchy user obtained
KVM API 12 and exit reason 5. No launcher CPU or runtime flags were changed.

See [the physical validation record](evidence/MAC-PARITY-2026-09-21.md) for artifact
identity. A subsequent r17 engineering-runtime pass booted Linux
`7.2.6-arch2-1` and a static PID 1 through nested QEMU 11.1.1 as the normal guest
user. QMP confirmed KVM present and enabled; both poweroff and reboot requests
exited cleanly. QEMU was supplied as a temporary test bundle, without installing
packages. See [the continuation evidence](evidence/PINCH-NESTED-2026-09-21.md).

This is a real kernel/userspace boot, not a full distribution or a nested
storage/network workload. Intel/Core Ultra, CPU rendering, other Windows builds
and configurations with different Hyper-V/security features remain separate
acceptance cases.

## Diskless Linux acceptance fixture

Build the tiny initramfs on an x86_64 Linux development host with a static C
toolchain:

```sh
python3 scripts/release/make-nested-linux-fixture.py /tmp/nested-initramfs.cpio.gz
```

Supply that initramfs, a compatible Linux kernel, QEMU and its firmware to the
Omarchy guest. Run as its ordinary user:

```sh
python3 check-nested-linux.py --qemu /path/to/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz --initramfs /path/to/nested-initramfs.cpio.gz \
  --firmware /path/to/qemu/share
```

The probe explicitly requires `accel=kvm` and confirms it over QMP. It attaches
no disks or network devices, bounds each boot to 60 seconds, checks the PID 1
readiness/exit markers, and requires a clean QEMU exit. The reboot case uses
`-no-reboot`: it proves a nested guest reboot request exits correctly, rather
than an unattended reboot loop. The fixture contains no shell or package manager.
