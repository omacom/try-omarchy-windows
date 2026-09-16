# Runtime command ownership repair, September 14, 2026

The candidate runtime `4.0.3-4` fixes the duplicate Neovim helper ownership in
[#116](https://github.com/omacom/try-omarchy-windows/issues/116). The runtime
package now takes commands from the materialized Omarchy command directory,
rather than collecting every `/usr/bin/omarchy-*` file. Registration and fresh
guest smoke checks reject an inconsistent package database. Compatibility revision
21 ensures existing v18 guests receive the new package repository even when the
external kernel is unchanged; without that bump the initramfs can skip the overlay.

## Existing-guest migration

Dropping duplicate ownership alone is insufficient: a real v18 upgrade test
showed pacman removing both helper paths when replacing the old runtime package.
The package now includes upgrade callbacks that retain the installed helpers
before removal and restore missing paths afterwards. This preserves the regular
setup script, the refresh symlink and administrator edits. A helper already
installed by another package in the transaction is left in place.

Recovery copies use a root-owned mode-0700 directory at
`/var/lib/try-omarchy/runtime-ownership-recovery`. Copies are staged before being
renamed into place. A successful restore removes its retained copy; an interrupted
transaction can retain recovery files. Reinstalling the corrected runtime runs
the restore callback again. This mechanism touches only the two previously shared
helper paths and never removes a pacman lock.

## Validation

A disposable copy of the checksum-verified v0.0.18-preview image was booted with
KVM, four vCPUs and 4 GiB RAM. The production candidate registration script built
its package from a staged copy of the released runtime and package database.
Removing only the old runtime's staged database registration modeled fresh
registration with dependency packages already installed.

The successful run verifies:

- Fresh runtime registration leaves `pacman -Dk` clean.
- The archive omits `usr/bin/omarchy-nvim-refresh` and `usr/bin/omarchy-nvim-setup`.
- Upgrading the actual guest from `4.0.3-3` to `4.0.3-4` leaves the database clean.
- Both helpers are owned only by `omarchy-nvim`; their hashes and the refresh
  symlink survive, including an administrator-style edit to the setup script.
- All 2,011 runtime package files are present and a user-document hash is unchanged.
- Another boot with the released v18 kernel/initramfs preserves those results.

The released Neovim package already reports a missing
`/etc/skel/.config/nvim/lua/plugins/theme.lua`. Tracked separately in [#119](https://github.com/omacom/try-omarchy-windows/issues/119).
The test requires its missing-file
diagnostics to remain exactly unchanged; it does not claim that package is wholly
intact. This is separate from the two helper ownership errors repaired here.

Nine unprivileged packaging/migration tests cover archive membership, invalid
command links, database-check failure, regular-file/symlink preservation, replay,
existing replacement files, a missing helper, absent shared ownership and unsafe recovery-directory
links. The complete reconstructed guest contract and existing smoke parser tests
also pass.

The local run verifies production packaging and upgrade behavior against the
released image, not a complete rebuilt factory image or Windows desktop acceptance.
The first full build attempt stopped at package-lock verification because five
Arch versions had advanced: libadwaita, libde265, libtirpc, qt6-declarative and
tzdata. Patch 0067 updates only those five pins to the exact transaction reported
by CI run 34903820728, matching the signed updates tested in the #90 investigation.
No lock verification was bypassed. The full build/boot rerun is recorded below.

## Reproduction and evidence

Apply the repository's complete patch series to the locked guest builder, then:

```sh
python3 scripts/release/smoke-runtime-ownership.py \
  /path/to/verified-v18-artifacts /path/to/patched-builder /path/to/new-work-directory
```

The verified artifact directory must include decompressed `rootfs.ext4`, the
kernel, initramfs and build spec. The runner keeps its disposable disk and logs;
never execute its guest fixture scripts on a valued installation.

Local evidence: `/home/bts/Projects/try-omarchy-evidence/issue116-run04`.

| Log | SHA256 |
| --- | --- |
| 01-upgrade.log | `2e19d5073d33aa8324727eeb22f49e9b4ad14d4aa229aae1b590911d76b3c808` |
| 02-reboot.log | `14375532d22250c215cab7c224069d52d2ab0577c269a519e9cb876241b5b3d3` |

Earlier runs retained the observed file-removal failure and intermediate migration
failures. The successful run above includes the preservation behavior. A subsequent
unprivileged regression also verifies that an already-missing helper does not
prevent preserving the other one.
Published v18 assets remain unchanged; this repair requires a new guest payload
and installation of the updated runtime package inside an existing guest.

## Complete factory build and boot

[CI run 34904145068](https://github.com/omacom/try-omarchy-windows/actions/runs/34904145068)
passed on application commit `9bc4f52`, including all launcher checks, 114 guest
contract tests and the full locked factory build and KVM boot. The smoke reports
`runtime-package=4.0.3-4`, `package-database=clean`, `pacman-unlocked=yes`,
`compat-version=yes` (revision 21) and an active update repository. Browser policy,
lock PAM, file transfers and the other existing smoke facts also pass.

Retained artifact ID: `10372531418`; artifact ZIP SHA256:
`ca37c4575df787632f3e7e28dcb83879aadc272df6eafdc0369937f7caf3cb27`.
This is an unpublished CI candidate, not a signed Windows release.

## Normal updater delivery from v18

The downloaded CI candidate's files matched its checksum manifest, including the
decompressed rootfs. With the strengthened `smoke-guest-upgrade.py` fixtures, all
five boots passed: seed a v18 guest, boot the candidate and run `omarchy-update -y`,
reboot the candidate, boot the old external image, then return to the candidate.
The kernel bytes are unchanged from v18, so delivery exercises the revision-21
compatibility change rather than relying on a kernel-version change.

The update installs runtime `4.0.3-4` through the image-provided repository.
Database consistency, exclusive Neovim helper ownership, helper hashes/symlink,
user document and configuration hashes, and installed-package preservation all
pass. Old-image boot does not downgrade the installed runtime. The updater's
package hooks report no execution errors. This remains headless KVM evidence,
not Windows launcher or graphical desktop acceptance.

Candidate initramfs SHA256:
`27ba7557109441eea0c9c6b04a54e3912f2c639bd170964cc0d82a4b9161be70`.
Compressed rootfs SHA256:
`8202b7f80850388aa79f45351922a8c87dbb42b3a48b0ad574278fd282cd898e`.
Decompressed rootfs SHA256:
`3537945357c088af347fc02c22c0febb8187c374f140ed4c2fbb988eaf8d50e6`.

Local evidence: `/home/bts/Projects/try-omarchy-evidence/issue116-full-upgrade`.

| Log | SHA256 |
| --- | --- |
| 01-seed.log | `a927b7bf95ba693c5570429de3fe5944dea5f385d8dd8c455a48afdb2995146c` |
| 02-upgrade.log | `2e3200b1bdbe9dcd7db7ed7bb7492bd81ab1c565efaabd8e0f3a1dea0c504f51` |
| 03-reboot.log | `597e8cbefff900abe36fe6a3312b4a0cee11957c972b69ad150cef401e5e8fd5` |
| 04-old-image.log | `66f91b1cd85ee4ef2180fdad05101315a7abd3c49922e1499c86f7c1db993a14` |
| 05-return-to-candidate.log | `5a772f24de2f6bf2109da230aa22bccb29a1e23e5dca7780b3812deab77dca42` |
