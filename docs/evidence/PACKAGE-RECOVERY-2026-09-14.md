# Package update and interruption investigation, September 14, 2026

The published v0.0.18-preview factory image starts without a pacman lock and
completes its system package update on the tested Linux/KVM host. A controlled
SIGKILL during a fixture package's pre-transaction hook leaves a stale lock that
survives reboot. Controlled removal of that known stale lock allows installation
to finish; another reboot preserves the installed fixture and user-file hash.

This narrows [#90](https://github.com/omacom/try-omarchy-windows/issues/90); it does
not establish the original reporter's cause or close Windows release acceptance.
The investigation also found a separate database ownership defect,
[#116](https://github.com/omacom/try-omarchy-windows/issues/116).

## Artifacts and host

- Published baseline: v0.0.18-preview, runtime package `4.0.3-3`, guest kernel
  `7.2.4-arch1-2`, compatibility revision 20.
- `SHA256SUMS` matched the released launcher's embedded pin:
  `023908807e48848c972462c10dc01a31193e240140d466e72699953cd27c3346`.
- Compressed rootfs SHA256:
  `e5495ccd5ec1fc8fc9631cb94c22441dc427a9138277a37356a96370bb9366bc`.
- Kernel, initramfs and build spec also matched that checksum manifest.
- Linux `7.2.5-3-omarchy`, QEMU `11.1.1`, KVM, four vCPUs, 4 GiB guest RAM;
  fresh disposable raw disk expanded to 24 GiB, headless multi-user target.
- Run with `scripts/release/smoke-package-recovery.py`; see
  [reproduction instructions](../GUEST-UPGRADES.md#package-lock-interruption-test).

## Results

| Check | Result |
| --- | --- |
| Fresh image and first boot contain no pacman lock | Pass |
| Normal `omarchy-update -y` package-update path | Five signed repository packages updated; command exited 0; lock absent afterwards |
| Headless desktop-shell restart | Warning: shell did not become ready; no graphical acceptance claimed |
| User-file hash after normal update | Unchanged |
| Competing package transaction while the fixture's real pacman lock is held | Refused; lock inode, size and modification time unchanged |
| SIGKILL of fixture pacman and its hook through their dedicated systemd service | Lock remains; no pacman process remains |
| Reboot and repository service restart | Stale lock remains; subsequent transaction is blocked |
| Recovery after checking test marker, no pacman process and no open lock holder | Fixture package installs; four package files present and expected data matches |
| Final reboot | No lock; installed fixture and user hash preserved |
| Package database integrity | Two pre-existing duplicate-ownership errors, unchanged through recovery; not a clean integrity pass |

The five updated packages were libadwaita, libde265, libtirpc, qt6-declarative and
tzdata. Network repository state can change on later runs.

## Separate ownership defect

Before the first update, `pacman -Dk` reports:

```text
error: file owned by 'omarchy-nvim' and 'try-omarchy-runtime': 'usr/bin/omarchy-nvim-refresh'
error: file owned by 'omarchy-nvim' and 'try-omarchy-runtime': 'usr/bin/omarchy-nvim-setup'
```

The reconstructed builder's `register-omarchy-runtime.sh` collects all
`/usr/bin/omarchy-*` commands, including the other package's helpers, then uses
`pacman -U --dbonly`. Its `-Qk` check does not catch duplicate ownership.
#116 tracks correction and existing-guest migration; no package-ownership fix is
included in this investigation.

The initial run stopped on this unexpected database failure. The completed run
records the post-update diagnostics and exit status, then requires exactly the
same results through interruption/recovery. That establishes no additional
reported database damage, not absence of pre-existing defects.

## Evidence and limits

The completed run has success markers for all four phases. Raw serial logs and
the disposable disk remain under the maintainer's local evidence directory
`/home/bts/Projects/try-omarchy-evidence/issue90-v18/run02`; they are not release
assets. Log hashes:

| File | SHA256 |
| --- | --- |
| 01-fresh.log | `bb9f999c3b61379bbcdf5f79e26dcd5f8eeba200f18b56e78b18c8828a39e9a1` |
| 02-interrupt.log | `16893f327e85771417cd5a5267a51bf97624b0ae8544a9364c4eb04be4c6e184` |
| 03-recover.log | `1b2f8c84f529f3540220d620e5231081bcbe4bb2b4684507e19e72395d967f52` |
| 04-reboot.log | `591324d8b83f433d54f10fdc0f626ada1e4a42770bd6caba9cb890701a3b0369` |

The scripts kill before fixture package writes, then shut the guest down normally.
They do not simulate host power loss, interruption during extraction or scriptlets,
Windows/WHPX behavior, launcher rollback, or preview-to-stable migration. Recovery
is manual and explicitly limited to the test-owned transaction; no automatic lock
removal is added to the product. #90 remains open for the original cause and the
remaining package-interruption cases.
