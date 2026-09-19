# Resume here

Updated September 19, 2026. PR #135 merged as `58f5b70`; PR #136 merged as
`e1341df`. **v0.0.20-preview is published and Latest**, from source `98cce48`.
[Release](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.20-preview)
| [successful publication](https://github.com/omacom/try-omarchy-windows/actions/runs/35470832033).

The candidate is built from `946521b`, with guest compatibility **29**. It adds a
filesystem capability check before consuming a drop ticket. This fixes the
shared-mount timestamp failure found during fresh Windows testing: unsupported
folders use Downloads with a notification. The fix passed physical shared-folder
fallback and normal-folder delivery with matching hashes and duplicate preservation.

Linux/Windows CI, all 157 guest tests, the full image build/fresh boot and the
five-boot v19 upgrade/old-image/return sequence passed. Windows camera, microphone,
normal use and quiet drops have user acceptance. Full Windows backup/restore,
forced update rollback, successful upgrade/reboot and 52 native storage/update
tests passed. Fresh compatibility-28 desktop, camera and Vulkan playback passed
with the signed diagnostic launcher. The exact compatibility-29 package also upgraded Windows successfully, with
matching helper hashes and preserved files. A clean compatibility-29 guest also
passed accelerated desktop, module and packaged shared-folder drop checks.

Exact hashes and validation boundaries are in
[the candidate record](evidence/PREVIEW20-CANDIDATE-2026-09-19.md).
Final signed v20 acceptance passed on the physical Windows laptop: GPU boot and
reboot, preserved fixtures, unchanged completed-repair timestamps, 30 distinct
camera frames and native file delivery with duplicate preservation. The published
launcher has valid Authenticode and v20 version resources. Tagged and Latest
assets and both signed update feeds passed through current and legacy URLs.
The public download also booted on the laptop with public payload URLs and
preserved all fixtures. Release-commit CI is green.

Physical sleep/wake remains untested. The user declined that test and accepted
proceeding with this preview; the public release notes state the limitation.
Sleep/wake and the broader hardware, storage and stable-update checks remain v1
work. This preview release does not certify those paths.
See [LAPTOP-PASS.md](LAPTOP-PASS.md) and [RELEASING.md](RELEASING.md).

## V1 validation follow-up

The [September 19 v1 laptop pass](evidence/V1-LAPTOP-2026-09-19.md) closes the
repository-transfer update gate: the original signed v7 launcher followed its
built-in legacy URL to public v20, preserved the guest fixture and committed only
after readiness. An interrupted attempt first restored v7 and preserved that file.

A full v20 snapshot was restored into a separate installation. The restored
copy booted on the GPU, retained all three fixtures, grew to 32 GiB and did not
shrink when a lower capacity was requested. Reboot retained that capacity and
the fixtures. Windows refused a competing write
handle during snapshot creation. The laptop currently runs the restored test
copy; original test installations, snapshots and evidence are retained.

Continue with stable-update transitions and the remaining storage/migration
checks. Full Hyper-V is disabled; a proposed temporary enable/test/restore cycle
requires Windows reboots and has not been authorized. SSHD has Manual startup,
so remote-reboot preparation must account for it. Do not reboot during disk tests
or assume that an interactive Windows sign-in will return automatically.

The remainder is historical context from September 15-17. Current GitHub state
and the dated checkpoint above supersede its PR counts and candidate references.

## Earlier handoff

Updated September 15, 2026, after merged PRs #115, #117, #118, #120, #121, #123,
#125 and #126. The user is working toward an official v1 and approved a focused
reliability, recovery and hardware-validation plan. Use
[V1-READINESS.md](V1-READINESS.md) and
[issue #77](https://github.com/omacom/try-omarchy-windows/issues/77) for scope and
remaining gates. The older full-feature plan is historical, not the v1 requirement.

## Repository and release state

- Repository: `omacom/try-omarchy-windows`; primary checkout:
  `/home/bts/Projects/try-omarchy-windows`; base branch: `master`.
- Latest implementation merges: `8d3fbe8` (the v19 re-pin), on top of
  [#125](https://github.com/omacom/try-omarchy-windows/pull/125),
  [#126](https://github.com/omacom/try-omarchy-windows/pull/126) and
  [#123](https://github.com/omacom/try-omarchy-windows/pull/123). Start from current
  `origin/master`; inspect local status before switching branches. Other checkouts
  can have unrelated work.
- Public Latest is [v0.0.19-preview](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.19-preview),
  published September 16 via [Release run
  35043448976](https://github.com/omacom/try-omarchy-windows/actions/runs/35043448976).
  It supersedes v0.0.18-preview (September 13). The launcher pin, `currentVersion`,
  the version resource, and the launcher's public `Latest` all point at v19, and
  `README.md`/`docs/TESTING.md` were updated to match. The legacy feed URL
  (`tsouth89/try-omarchy-windows`, now a redirect to `omacom`) serves v19
  `update.json`/`update-v2.json`, so existing installs update normally.
- **Context on the master breakage.** Master had been left pinned to the
  unpublished v0.0.19-preview draft, which broke fresh builds from `master` with an
  HTTP 404 on the graphics runtime (GitHub #124). #126 reverted the pin to v18 to
  restore it; #125 added `validate-pin.py --require-public` and enabled it in CI so
  this cannot merge silently again. Publishing a preview requires the pin on
  `master` first (the workflow guard needs `refs/heads/master` and `publish`
  verifies the source pin against the draft manifest), so master's pin step is red
  between the re-pin and the release becoming public. That window closed when v19
  published. See [RELEASING.md](RELEASING.md).
- The published v19 guest is Omarchy 4.0.3 with runtime `4.0.3-4` and compatibility
  revision **22**. Unreleased `master` now carries compatibility revision **26**
  and adds, in order: `0070` early-boot orphaned-pacman-lock recovery for
  [#90](https://github.com/omacom/try-omarchy-windows/issues/90) (revision 23),
  `0072` the Windows camera bridge with v4l2loopback (revision 24), `0073` the
  direct file-drop helper (revision 25), and `0074` delivering a direct drop into
  the window under the point (revision 26). `0071` refreshed the drifted Arch lock
  (`linux` 7.2.4→7.2.6). Existing guests receive all of this on the next launcher
  update that carries the revision-26 initramfs; none of it is in v19.
- **v19 physical-test note.** The extensive Windows laptop acceptance
  ([September 13](evidence/WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md)) was for the v18-era
  artifacts (runtime r7-r15, compatibility-19/20 guest). v19 was validated on
  Linux/KVM and CI, not physically. Publication was accepted because the guest
  kernel (`vmlinuz-linux`) and the GPU runtime
  (`winq-emu-alpha10-portable.zip`) are byte-identical between v18 and v19: the
  only differences are ordinary guest package bumps (`try-omarchy-runtime`
  `4.0.3-3` to `4.0.3-4`, `libadwaita`, `libde265`, `libtirpc`, `qt6-declarative`,
  `tzdata`), the rebuilt initramfs/rootfs carrying compatibility 22, and metadata.
  None of the physically-validated surfaces (kernel boot, virgl/Venus graphics,
  input, audio) changed. The five-boot normal-updater preservation run covered the
  changed guest path against the v18 baseline. Do not treat this as physical
  acceptance of v19; it is an evidence-based exception for a candidate that did
  not touch the physically-risky surfaces.
- **Omarchy 4.0.4 (upstream, September 15)** ships a bespoke `linux-omarchy`
  kernel, a webcam fix, and package/hardware fixes. Our guest boots its own external
  `vmlinuz` and holds the guest `linux` package, so the headline kernel change does
  not apply to Windows guests, and the webcam fix is irrelevant without a camera
  bridge. Desktop/package changes reach existing guests through **Update > Omarchy**
  where supported. No factory rebuild is required for 4.0.4; fold an Omarchy bump
  into the next factory candidate rather than a v19.1.

## Completed in the September 16 session

- **Docs and repo cleanup.** `docs/` went from 32 files to 18: evidence moved to
  `docs/evidence/`, superseded session/candidate notes deleted, `SESSION-RESUME.md`
  renamed to `HANDOFF.md`, and `sign.ps1` repointed at `RELEASING.md`. Merged local
  and remote branches pruned; local build junk removed.
- **#90 orphaned-lock recovery** (guest `0070`, revision 23) with unit, contract,
  and KVM regression coverage. **Lock refresh** (`0071`) so factory builds resolve.
- **Webcam** — guest `0072` (v4l2loopback at `/dev/video42`, on-demand bridge) and
  launcher transport (`virtio-serial` port `dev.tryomarchy.camera` on port 4453)
  plus a Media Foundation capture source with a synthetic test mode. Unverified at
  runtime.
- **Direct in-app drops** — guest `0073`/`0074` (`try-omarchy-drop` targets the
  window under the point; the transfer ticket carries the point and the window
  stays hidden until delivery fails) and launcher drop capture on the QEMU window.
  Unverified at runtime.
- **Pause-on-host-sleep** — tray power broadcast pauses the guest (QMP `stop`) and
  resumes it (`cont`) ahead of the existing clock re-sync.
- **CI fix** — `smoke-guest.py` now derives the expected compatibility revision
  from the newest guest patch instead of hardcoding 22.

## Completed in the September 15 session

- **#90 orphaned-lock recovery (unreleased).** Guest patch
  `guest-build/0070-Recover-from-an-orphaned-pacman-lock-left-by-an-interrupted.patch`
  adds `try-omarchy-pacman-lock.service` and
  `/usr/local/lib/try-omarchy/clear-stale-pacman-lock`. It runs once at early boot
  and removes `/var/lib/pacman/db.lck` only when it cannot belong to a live
  transaction, logging the reason; it never runs pacman or repairs the database.
  Compatibility revision moved 22 → 23 and the new files are in the compat overlay
  so existing disks receive them. New unit tests
  (`guest/tests/test_pacman_lock_recovery.py`) pin the removal and every
  do-not-remove case; `guest/tests/verify.py` asserts the delivery; the KVM
  `smoke-package-recovery.py` recovery phase now asserts automatic removal and a
  following normal transaction. `scripts/release/build-guest.sh --contract-only`
  passes. Validated on Linux/KVM: a factory image built from this patch passed all
  four `smoke-package-recovery.py` phases; the recover phase logged
  `removed stale pacman lock /var/lib/pacman/db.lck (lock predates this boot and no
  pacman process owns it)`, left `pacman -Dk` diagnostics unchanged, and installed
  the fixture normally with no manual removal. That build refreshed the drifted
  Arch lock (`linux` 7.2.4→7.2.6, `uwsm`, `libpcap`) only to build; the refreshed
  lock was not committed, so master still pins the v19 packages and this remains an
  unreleased fix.
- **v0.0.19-preview published.** After the pin fix below, the pin was restored to
  v19 (`8d3fbe8`) and the `publish` phase ran on `master` ([run
  35043448976](https://github.com/omacom/try-omarchy-windows/actions/runs/35043448976)).
  Verified public: `releases/latest` serves v19, `TryOmarchy.exe`,
  `TryOmarchy.exe.sha256`, `SHA256SUMS`, and both signed update feeds return 200,
  and `update-v2.json` carries version `v0.0.19-preview`, manifest
  `a4d2f54d…8756e`, and launcher sha256 `90a74976…`. The draft physical test was
  intentionally not run; the justification and residual risk are recorded in the
  release-state section above.
- **Master builds restored; #122/#124 closed.** The launcher pin had been moved to
  the unpublished `v0.0.19-preview` draft, so a fresh build from `master` failed
  with an HTTP 404 on the graphics runtime (#124). #126 reverted the pin,
  `currentVersion`, and the version resource to `v0.0.18-preview`, verified against
  the live published `SHA256SUMS`. #125 added `--require-public` to
  `scripts/release/validate-pin.py` and enabled it in CI, so a pin bump to a
  release that is not publicly reachable now fails the build. #123 fixed #122:
  `validateMovePath` runs the full link-and-stream check on the install path itself
  and a links-only check on its ancestors, so an unrelated NTFS stream on a folder
  like the user profile no longer blocks an installation move. All three were
  external contributor PRs (Rovetown) and all required CI passed before merge.
- **#121 merged; #119 closed:** the factory builder replaced `/etc/skel/.config`
  and rebuilt the Neovim skeleton from `/usr/share/omarchy-nvim/config`, which
  omits `lua/plugins/theme.lua`. The `omarchy-nvim` package seeds that path in
  `/etc/skel` as a relative symlink to the active theme's generated `neovim.lua`,
  so new accounts opened Neovim without the Omarchy colorscheme and
  `pacman -Qk omarchy-nvim` warned. Patch 0069 retains the packaged skeleton
  during materialization, compatibility revision 22 ships the link to existing
  disks, and `catch-up` restores it for users who lost it without replacing their
  own file. See [Neovim skeleton evidence](evidence/NVIM-SKELETON-2026-09-14.md).
- Validation passed: 117 guest contract tests (one optional skip) and 15 release
  unit tests; a manually dispatched CI run built the complete factory image and
  booted the instant account with `nvim-theme-skel=yes`, `nvim-theme-user=yes`,
  `omarchy-nvim-files=yes` and `compat-version=yes` (revision 22); the five-boot
  normal-updater regression passed against the checksum-verified v18 baseline and
  now asserts the exact theme-link target on the existing disk and in the instant
  account's home. These are Linux/KVM and CI results, not new physical Windows
  acceptance.
- An independent review found no blockers; its minor findings (fact gate
  revision, a dead fallback that could not build, the Hyprland precondition,
  missing target assertions, doc wording) were fixed and re-validated in the
  merged revision.

## Reusable candidate and local evidence

The complete guest candidate comes from
[CI run 34922457869](https://github.com/omacom/try-omarchy-windows/actions/runs/34922457869),
built on application commit `3005d89` (merged as `e7280fe`). Artifact
`guest-candidate` is retained by CI for seven days from September 15; a verified
local copy is kept at `issue119-candidate/`. Decompressed rootfs SHA256:
`fbff55d881ddfeea2679aa80ba578ef17427dd41ecf3dd6d55f33123698a3c01`.
The factory is Omarchy 4.0.3, runtime `4.0.3-4`, compatibility 22, kernel
`7.2.4-arch1-2`. Runtime r15 for Windows remains the published v18 binary; this
work changed the guest package, not the Windows QEMU runtime.

| Local path beneath `/home/bts/Projects/try-omarchy-evidence/` | Contents |
| --- | --- |
| `issue119-candidate/` | Verified complete candidate artifacts, including decompressed rootfs |
| `issue119-upgrade/` | Successful five-boot normal-updater run with the revision-22 link assertions |
| `issue119-prefix-smoke/` | Pre-fix compatibility-21 detection log for the missing link |
| `issue116-candidate/` | Pre-fix compatibility-21 candidate, kept for negative controls |
| `issue116-full-upgrade/` | Successful five-boot normal-updater test from the #118 work |
| `issue116-run04/` | Successful direct runtime-ownership package-upgrade/reboot test |
| `issue90-v18/artifacts/` | Verified published v18 baseline, including decompressed rootfs |
| `issue90-v18/run02/` | Successful package-lock interruption/recovery test and retained disk |
| `issue90-v18/builder/` | Reconstructed locked guest builder with patches through 0068 applied |
| `issue116-build-success.log` | Full successful factory-build/boot CI log from the #118 work |

The failed `issue116-run01`–`run03` investigation disks were removed on
September 15 to reclaim space; they were not successful candidate evidence.
`issue90-v18/run01` still retains a failed investigation run. Verify checksums
before reuse.

At handoff, no local `qemu-system-x86_64` test process remains. Free space was
about 15 GiB after validation; recheck before creating images. The Windows laptop
was not contacted or changed in these sessions, so its older free-space and
process observations are not current facts.

Reproduction entry points:

- `scripts/release/smoke-guest.py`: fresh factory boot; new `nvim-theme-*` and
  `omarchy-nvim-files` facts run from compatibility 22.
- `scripts/release/smoke-guest-upgrade.py`: normal updater and five-boot
  preservation, now with the exact theme-link assertions;
  [instructions](GUEST-UPGRADES.md#validation).
- `scripts/release/smoke-package-recovery.py`: controlled lock interruption;
  [instructions](GUEST-UPGRADES.md#package-lock-interruption-test).
- `scripts/release/smoke-runtime-ownership.py`: direct packaging/upgrade regression;
  [instructions](evidence/RUNTIME-OWNERSHIP-2026-09-14.md#reproduction-and-evidence).

## Remaining work and next steps

1. **#90 follow-through.** The orphaned-lock recovery (guest patch `0070`,
   compatibility revision 23) addresses the user-facing symptom: an interrupted
   update no longer blocks later updates until the user removes the lock by hand.
   It removes a lock only when it is provably orphaned (regular file, no
   pacman/alpm process, no open holder, mtime older than this boot) and logs the
   decision. Active locks stay protected. Open: the original reporter's exact
   cause, and interruption during package writes or power loss mid-extraction,
   which the controlled pre-transaction KVM test does not reproduce. Run
   `scripts/release/smoke-package-recovery.py` on the exact next candidate.
2. **v19 physical smoke (optional, for the record).** v19 is published without the
   draft physical test, accepted because the guest kernel and GPU runtime are
   byte-identical to v18 (see the release-state section). If convenient, run the
   published v19 launcher on the Windows laptop with a copied data directory to
   confirm the revision-22 compatibility repair runs once, the desktop and files
   survive, and reboot/poweroff are clean, then record it as v19 acceptance. Not a
   blocker. Decide whether v19 serves as the `LEGACY_UPDATE_BRIDGE_TAG` if v1 is
   next.
3. **Next factory candidate.** Fold an Omarchy version bump and any remaining guest
   fixes into the next candidate (likely the v1 candidate) rather than a separate
   v19.1. Omarchy 4.0.4's headline kernel change does not apply to our external-kernel
   guest.
4. **Physical coverage.** Intel/NVIDIA, full Hyper-V/Core Ultra, advertised Windows
   versions, sleep/resume, mixed-DPI displays, remote input, device switching and
   microphone behavior remain open. Use [TESTING.md](TESTING.md). Also complete
   native Omarchy export/restore acceptance and final support/distribution docs.

Open issues at this checkpoint: #77, #90. No open pull requests; #111 (borderless)
was closed without merging and remains optional for v1. Recheck GitHub before
acting; counts and states can change.

Webcam capture and direct in-app drops are now implemented on `master` (guest
patches `0072`–`0074` plus the launcher side) but are **unverified at runtime**:
the Media Foundation capture has never run, and the camera channel and direct-drop
delivery need a VM/laptop pass. Treat them as pending verification, not shipped.

Accelerated RAM resume is not a v1 candidate. The app has disk snapshots and
rollback, and a `migrate`-based saved-session path that refuses when the runtime
reports migration blockers (which the GPU path does). Pause-on-host-sleep is
implemented instead (tray receives WM_POWERBROADCAST, QMP `stop`/`cont`).

Portable mode stays experimental. True bridged networking (guest on the LAN with
its own address), ARM64 and booting a physical install remain outside the accepted
v1 scope. Do not resume the old eight-feature plan as though all of it blocks v1.

## Earlier Windows evidence and recovery context

Read [the September 13 laptop acceptance](evidence/WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md)
for physical AMD evidence and the final signed v18 publication record. Earlier
sections describe intermediate failures; use the later explicit retests.

The Windows record includes cleanup operations rejected by automatic approval
review. Their exact targets are retained there; do not retry those deletions
through another route. Re-inventory the host and recoverable data before further
large portable tests. A current session's user instructions take precedence over
historical plans, but old evidence is not permission for new publication, cleanup
or messages to other people.

Use this document and the v1 tracker for the current direction. Historical session
and candidate notes were removed from the repo during the docs cleanup; git history
retains them if an old path or recovery detail is ever needed.
