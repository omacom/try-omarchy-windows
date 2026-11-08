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
pacman. Besides Omarchy's own files, the runtime depends on the packages the
Try Omarchy integration needs (`integrationDepends` in the guest spec: GTK and
gtk4-layer-shell, python-gobject, polkit, openssl, socat, jq, wl-clipboard, the
fcitx5 input packages, CJK fonts and a few tools). When a newer image adds one,
the runtime's package release goes up, so the next update installs it on disks
created by older images. Apps a user removed on purpose are not on that list
and stay removed. System configuration shipped by the runtime uses pacman's backup
handling, so local edits can be retained with a `.pacnew` file for review.
Personal files stay on the existing disk. The launcher continues to supply the
external kernel and matching modules; the guest's linux package stays held.

Compatibility revision 54 installs pacman hooks before the desktop starts on
existing disks. When a transaction replaces Omarchy files, the pre-transaction
hook pauses config autoreload in every running Hyprland instance. A temporary
systemd service records each instance's previous value and watches pacman's
process identity. The post-transaction hook asks it to reload the completed
config once, then restore the recorded values. If pacman fails or is killed,
the service restores them when the process exits, even though pacman skips
post-transaction hooks on failure. State lives only under `/run`, and no service
is enabled for the next boot. Headless transactions need no running compositor.
Both hooks are best-effort: setup or cleanup failures are printed to pacman
and do not cancel the package update. Failed setup asks the guardian to restore
any partially paused sessions; setup waits at most five seconds for readiness.

The hooks protect **Update > Omarchy**, its conflict retries, and direct pacman
transactions. They prevent the config watcher from loading a temporarily absent
`bootstrap.lua` while pacman replaces the runtime. An instance that already had
autoreload disabled keeps that setting after the explicit reload.

Compatibility revision 55 repairs the default package-conflict handler before
login so **Update > Omarchy** can adopt the night-light helper and shader that
compatibility catch-up delivered before the runtime package owned them. It
accepts only those two unowned paths, rechecks pacman ownership, and keeps their
previous contents under `/var/lib/omarchy/replaced`. If the retry fails before
installing either file, the handler restores its previous copy. Unrelated
conflicts and files owned by another package still stop the update. Customized
or linked conflict handlers are preserved. Runtime `4.0.4-6` carries the same
repair for fresh installations and subsequent updates.

Compatibility revision 56 delivers the audio bridge that retries the Windows
device catalog until PipeWire's QEMU transport is ready. Existing v0.3.0 through
v0.9.0 disks and revision-55 disks receive the corrected file before login,
without requiring **Update > Omarchy** or a kernel change. Fresh images include
the same bridge. Runtime `4.0.4-7` advances the package release for subsequent
runtime updates.

Compatibility revision 57 delivers a guest-specific `install/config/snapper.sh`
before login. The guest uses ext4 and has neither Snapper nor a Limine bootloader,
so this setup step prints a message and completes without changing snapshot
configuration or services. Omarchy migration `1781984677` can then finish even
on disks where its completion marker is missing. Other migrations still fail
normally on errors. Runtime `4.0.4-8` includes the same setup step for fresh
installations and subsequent updates. Resetting the disk is not required.

Compatibility revision 58 delivers the clipboard bridge before login on existing
disks, including revision-57 installations. Large Windows clipboard images use
fixed-length frame checks and stream through the base64 decoder instead of
stalling in shell suffix matching. Fresh images carry the same bridge. The bridge
is supplied by the compatibility overlay, so no runtime package bump or
**Update > Omarchy** is required. Resetting the disk is not required.

Compatibility revision 59 delivers the audio bridge that keeps volume sync
working when the raw QEMU transport device is chosen in Omarchy's audio panel.
Existing disks, including revision-58 installations, receive the corrected
bridge before login. While the Windows endpoint mirror is active, a default
sink or source that points at the raw VirtIO transport is switched back to the
mirror for the Windows device that is currently selected; other guest devices
are left alone. Fresh images carry the same bridge, which the compatibility
overlay supplies without a runtime package bump or **Update > Omarchy**.

Compatibility revision 60 repairs the shell palette before login after Qt 6.12
is installed. It fixes the missing top bar and black Win+K / Win+Space menus on
existing disks, including revision-59 installations. The repair runs after
older shell repairs and rewrites only files matching reviewed old digests;
customized, missing, linked and already-fixed files are kept. Older reviewed
variants receive only the palette namespace change, keeping their shell logic.
Fresh images carry the same qualified palette references. Runtime `4.0.4-9`
packages the fix for subsequent **Update > Omarchy** transactions. The prepared
v0.11.0 guest artifacts must be rebuilt from this fix before release.

Compatibility revision 61 delivers the QEMU CPU cursor-buffer profile before
login. Catch-up refreshes only an unchanged default monitor configuration and
keeps user edits and symlinks. It also offers the patched aquamarine archive in
the authenticated local repository. The next **Update > Omarchy** installs
runtime `4.0.4-10` and its minimum `aquamarine>=0.15.1-1.1` dependency; no package
transaction runs during boot publication.

The managed rebuild keeps Arch's package name, dependencies and SONAME provide.
The local repository is after Arch. An unchanged Arch `0.15.1-1` keeps the
installed patched `0.15.1-1.1`. A newer Arch release or version replaces it in
the ordinary update transaction, including a new SONAME required by Hyprland.
No version hold or custom provider blocks the upgrade. Until we re-pin the new
source, the cursor can return to being drawn within desktop frames. The host
cursor presentation fix is separate from this guest groundwork.

To re-pin, refresh the guest transaction lock, review the hotspot patch on the
new upstream source, and update `supplyChain.aquamarine` source, patch and
builder digests, Arch version, SONAME and provide. Use the new Arch version plus
`.1` as the local package version, raise the runtime package release and compat
revision, and repeat contract, real pacman transaction and image build/boot
checks. The build fails if source, build-tool pins, metadata or SONAME drift.

Compatibility revision 68 moves existing disks to Omarchy 4.5.0. The next
**Update > Omarchy** installs runtime `4.5.0-14`, mise 2026.10.25 and the new
desktop packages (Omasnap, Disktree, vi, xorg-xsetroot), then runs Omarchy's
migrations. Those replace Tensaku with Omasnap and dua-cli with Disktree, and
turn earlier mise wrappers into lazy shims. Migrations for apps the image leaves
out (OWE, Hype, Monologue, Signed, Gliff, floating titlebars, Mozc and the
Chinese add-ons), the repository reorder, the kernel switch and the BBR and ARP
sysctls print a message and change nothing, so updated and fresh disks have the
same packages. Omarchy 4.5 also removes orphaned packages without asking during
the update; packages the guest needs are explicit or runtime dependencies, so
they are not orphans.

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

## On-demand factory images (unreleased)

Feature-aware launchers stage kernel, initramfs, guest metadata and Windows
runtime updates without requesting `rootfs.ext4` or `rootfs.ext4.zst`. Creation
and explicit reset acquire the exact selected release's template, authenticated
by the complete pinned SHA256SUMS. Standard raw disks are independent after
creation. Portable disks retain their original backing until verified detachment
succeeds before publishing any new runtime or guest payload.

Component rollback restores the previous boot files, runtime and their recorded
pins. An independent disk stays independent, and rollback needs no new factory
image. This does not undo completed guest pacman transactions. The v0.9 portable
launcher's first hop remains unchanged and still acquires a full payload. Its
full previous installation is retained for automatic legacy-launcher recovery.
Manual downgrade of a template-free installation to a mandatory-template
launcher is unsupported; use its full compatible backup or distribution.

Legacy full installations can grow using their verified original template and
reset offline using the factory hash recorded in their receipt, even without
cached SHA256SUMS. A small raw disk with uncertain completeness now stops with
a recovery error instead of being automatically rebuilt. Its bytes are kept. Restore verified
original metadata or a backup, or explicitly use Start fresh to retain the old
disk while preparing a replacement.
