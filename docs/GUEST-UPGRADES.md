# Updating an existing guest

The launcher can deliver a newer Omarchy runtime without replacing the writable
VM disk. After updating the launcher and guest image, open **Update > Omarchy**
inside the guest to install it. Resetting the installation is not required.

The authenticated image carries the local package repository in its initramfs.
At boot, a service checks the payload hashes, copies new package archives, and
replaces the repository database atomically. It does not run pacman or modify its
lock. Old archives remain available to transactions that read the previous
repository. An older image cannot lower the repository's runtime version.

The normal Omarchy updater installs the runtime and its dependencies through
pacman. System configuration shipped by the runtime uses pacman's backup
handling, so local edits can be retained with a `.pacnew` file for review.
Personal files stay on the existing disk. The launcher continues to supply the
external kernel and matching modules; the guest's linux package stays held.

If repository publication fails, inspect:

```sh
journalctl -u try-omarchy-update-repository.service
```

Fix the reported cause, such as insufficient disk space, then retry:

```sh
sudo systemctl restart try-omarchy-update-repository.service
```

A failed package transaction must be diagnosed through the normal updater. The
publisher never removes `/var/lib/pacman/db.lck`. Keep a stopped-VM backup before
release-candidate testing. This update mechanism is not a backup or a rollback of
an already installed desktop.

Older previews also copied the build account's ownership onto some system paths.
A boot service repairs only the paths supplied by the image overlays, without
following symlinks or recursing into personal files. It also normalizes unchanged
bundled icon filenames to the names expected by desktop files. This prevents
system and icon-cache hooks from failing during package updates.

## Validation

On a Linux machine with KVM, use verified release artifacts and a newly built
candidate. Decompress the baseline `rootfs.ext4.zst` first. The work directory
must be new, and the test retains its disposable disk and logs for inspection:

```sh
python3 scripts/release/smoke-guest-upgrade.py \
  /path/to/older-release /path/to/candidate /path/to/new-test-directory
```

The test provisions an older image, seeds preservation fixtures, upgrades it,
reboots it, boots it with the older external image, and returns to the candidate.
It checks package versions, user files, an edited system configuration, installed
packages, busy-lock handling, repair services, and repeated publication. It does
not modify either input image. Network access is required for the normal signed
Arch repository updates.


The guest contract suite tests corrupt and incomplete payloads, interrupted
publication, retry, repeated publication, downgrade rejection, archive conflicts,
and unsafe filesystem paths. Release validation must also boot a copy of a real
older image, perform the package upgrade, and reboot with preservation fixtures.
Windows launcher rollback and graphical acceptance remain separate release gates.

### Candidate validation, 2026-09-12

Baseline: the published `v0.0.14-preview` factory image, with its checksums
verified against the launcher's pinned checksum list. Candidate: guest patch
0046 on top of the Omarchy 4.0.3 work in #91, runtime `4.0.3-2` and compatibility
revision 14.

Passed on a disposable 24 GiB disk under QEMU/KVM:

- Provisioned the actual 4.0.2 image and seeded a document, user configuration,
  and modified system configuration with recorded checksums.
- Booted the candidate without changing the installed runtime first.
- Confirmed a test-owned pacman lock blocks an update and remains untouched.
- Ran the complete `omarchy-update -y` command successfully. Optional prompts
  timed out without being accepted. Package hooks reported no execution errors.
- Verified runtime `4.0.3-2`, media-tool dependencies, all previously explicit
  packages, and unchanged preservation fixtures. Repeated migrations succeeded.
- Rebooted the upgraded disk, booted it with the old external kernel/initramfs,
  and returned to the candidate. Each boot retained 4.0.3 and the fixtures.
- Confirmed repository publication can be repeated, system ownership is repaired,
  and the package database is unlocked after the update.

A separate fresh-image boot passed browser-policy repair and passwordless theme
policy checks, icon-cache generation, package availability, kernel-module
matching, and the readiness service. All 70 guest behavioral tests and 13 release
script tests passed, as did reconstruction from the complete guest patch series.

This is headless guest validation. It does not complete the Windows signing,
Hyper-V, interactive desktop, or launcher update/rollback acceptance gates.

## Package-lock interruption test

To investigate #90 independently of a launcher update, use the published release
artifacts verified against the launcher's pinned `SHA256SUMS`. Decompress the
factory image with `zstd -d --long=28 --sparse rootfs.ext4.zst -o rootfs.ext4`.
On a Linux host with KVM, run:

```sh
python3 scripts/release/smoke-package-recovery.py \
  /path/to/verified-release /path/to/new-evidence-directory
```

The runner copies the factory image to a new disposable 24 GiB disk and retains
that disk and four serial logs. Allow enough host space for the image and package
updates. Network access is needed for the fresh guest's normal Omarchy updater.
It checks the initial lock state, runs the updater, then installs a local fixture
package whose pre-transaction hook pauses while pacman holds its real lock. A
competing transaction must fail without changing the lock. The test kills only
that fixture's systemd service, powers the VM off, and reboots. On the next boot
`try-omarchy-pacman-lock.service` removes the orphaned lock on its own and logs
the reason, so the fixture then installs normally with no manual lock deletion.
User-file hashes must match, and package-database diagnostics and their exit
status must remain identical to the post-update baseline. Existing database
errors are printed and retained, not treated as a clean integrity result.

The recovery only removes a lock it can prove orphaned: a regular file, no
pacman/alpm process running, no process holding it open, and an mtime older than
the current boot. An active transaction's lock is never touched, and anything
ambiguous is left in place and logged.

The fixture scripts still deliberately kill a package transaction; never run them
directly on a host or a valued guest. This covers a controlled interruption
before package writes, the orphaned-lock auto-recovery, and the following normal
transaction. It does not cover power loss during extraction, a partially
installed system update, Windows launcher rollback, or the original reporter's
unknown interruption. Passing it does not establish those other cases.
