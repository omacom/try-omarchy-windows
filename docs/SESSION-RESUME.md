# Resume here

Updated September 15, 2026, after merged PRs #115, #117, #118, #120, #121, #123,
#125 and #126. The user is working toward an official v1 and approved a focused
reliability, recovery and hardware-validation plan. Use
[V1-READINESS.md](V1-READINESS.md) and
[issue #77](https://github.com/omacom/try-omarchy-windows/issues/77) for scope and
remaining gates. The older full-feature plan is historical, not the v1 requirement.

## Repository and release state

- Repository: `omacom/try-omarchy-windows`; primary checkout:
  `/home/bts/Projects/try-omarchy-windows`; base branch: `master`.
- Latest implementation merges: `608b824` ([#125](https://github.com/omacom/try-omarchy-windows/pull/125)), on top of
  [#126](https://github.com/omacom/try-omarchy-windows/pull/126) and
  [#123](https://github.com/omacom/try-omarchy-windows/pull/123). Start from current
  `origin/master`; inspect local status before switching branches. Other checkouts
  can have unrelated work.
- Public Latest remains [v0.0.18-preview](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.0.18-preview),
  published September 13. No newer signed launcher or public release was made.
- The launcher pin is back at **v0.0.18-preview** (#126) after master was left
  pointing at the unpublished v0.0.19-preview draft, which broke fresh builds from
  `master` with an HTTP 404 on the graphics runtime. CI now runs
  `validate-pin.py --require-public` (#125), so a pin bump to a release that is not
  publicly reachable fails the build instead of merging silently. Note that
  publishing v0.0.19-preview requires restoring the v19 pin on `master` first
  (the workflow guard needs `refs/heads/master`, and `publish` verifies the source
  pin against the draft manifest), so `master`'s CI will show the pin step red
  between that re-pin and the actual `publish`. That is expected; see
  [RELEASING.md](RELEASING.md).
- Unreleased master includes #113/#114 portable-copy and Windows publication-lock
  improvements, #118's runtime ownership repair, and #121's Neovim theme-link
  repair. These are code changes; the pinned download release is v18 again.
- The guest is Omarchy 4.0.3 with runtime `4.0.3-4` and compatibility revision
  **22** (was 21). Existing guests that take a newer launcher re-apply the
  compatibility overlay once.
- A **v0.0.19-preview** draft release is fully prepared ([Release run
  34941645832](https://github.com/omacom/try-omarchy-windows/actions/runs/34941645832)).
  The
  [signing-check run 34942547584](https://github.com/omacom/try-omarchy-windows/actions/runs/34942547584)
  produced the signed test launcher artifact for the physical draft test.
  `.github/release-notes/v0.0.19-preview.md` is committed, and every draft asset
  was downloaded and matched against the pinned `SHA256SUMS`; the draft digest is
  `a4d2f54dcafaaf57290789184938c439283e0931aafaa7742f743d7693a8756e`, which matches
  the (now reverted) source pin at `43faade`. The launcher's public `Latest` is
  still v18 until `publish` runs. Use [RELEASING.md](RELEASING.md); source merges and
  guest CI artifacts are not releases.
- To publish v0.0.19-preview, first restore the pin to v0.0.19-preview
  (`defaultReleaseURL`/`defaultSumsSHA256`/`currentVersion` and the version
  resource, digest `a4d2f54dcafaaf57290789184938c439283e0931aafaa7742f743d7693a8756e`),
  then run the physical draft test and the `publish` phase. After publication, bump
  the current-release references in `README.md` and `docs/TESTING.md` from
  v0.0.18-preview to v0.0.19-preview. Undo this revert (`git revert` of the #126
  content) is exactly that re-pin.

## Completed in the September 15 session

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
  own file. See [Neovim skeleton evidence](NVIM-SKELETON-2026-09-14.md).
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
  [instructions](RUNTIME-OWNERSHIP-2026-09-14.md#reproduction-and-evidence).

## Remaining work and next steps

1. **#90: remaining interruption/recovery investigation.** The original reporter's
   stale-lock cause is unknown. Tests cover SIGKILL before package writes, not
   power loss during extraction or scriptlets. Keep active locks protected; no
   automatic lock deletion was added.
2. **v0.0.19-preview draft physical test.** The draft and pin are ready; run the
   signed candidate from
   [RELEASING.md](RELEASING.md#test-the-draft-on-physical-windows) on the Windows
   laptop with a copied data directory and loopback payload. Confirm the desktop
   and files survive, the new external kernel boots, reboot and poweroff work, the
   revision-22 compatibility repair runs once, and a stopped first boot rolls back.
   Then run the `publish` phase. Do not call this a Windows update/rollback pass
   until those checks pass. Consider whether this candidate should serve as the
   `LEGACY_UPDATE_BRIDGE_TAG` if v1 is next.
3. **Physical coverage.** Intel/NVIDIA, full Hyper-V/Core Ultra, advertised Windows
   versions, sleep/resume, mixed-DPI displays, remote input, device switching and
   microphone behavior remain open. Use [TESTING.md](TESTING.md). Also complete
   native Omarchy export/restore acceptance and final support/distribution docs.

Open issues at this checkpoint: #77, #90. No open pull requests; #111 (borderless)
was closed without merging and remains optional for v1. Recheck GitHub before
acting; counts and states can change.

Portable mode stays experimental. Webcam capture, accelerated RAM resume,
arbitrary-app drops, bridged networking, ARM64 and booting a physical install
remain outside the accepted v1 scope. Do not resume the old eight-feature plan
as though all of it blocks v1.

## Earlier Windows evidence and recovery context

Read [the September 13 laptop acceptance](WINDOWS-LAPTOP-ACCEPTANCE-2026-09-13.md)
for physical AMD evidence and the final signed v18 publication record. Earlier
sections describe intermediate failures; use the later explicit retests.

The Windows record includes cleanup operations rejected by automatic approval
review. Their exact targets are retained there; do not retry those deletions
through another route. Re-inventory the host and recoverable data before further
large portable tests. A current session's user instructions take precedence over
historical plans, but old evidence is not permission for new publication, cleanup
or messages to other people.

[Archived session notes](SESSION-RESUME-2026-09-13.md) and
[Windows lab notes](WINDOWS-SESSION-HANDOFF.md) preserve paths and historical
recovery context. Use this document and the v1 tracker for the current direction.
