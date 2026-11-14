# Releasing Try Omarchy

Releases use a two-phase GitHub Actions workflow so the signed launcher can pin
the guest manifest produced for that same release without rewriting source code
inside CI.

## When to release

Ship a substantial release about every two weeks, built around what is ready
and with every CHANGELOG entry's user check passed. Fixes keep merging to
`master` in between; only publishing waits.

Cut a patch release between them only when users cannot start or boot
Omarchy, lose data, are exposed to a security issue, or hit a regression from
the last release that affects many people. Everything else waits for the next
substantial release.

## One-time setup

The `release` GitHub environment is restricted to the `master` branch. Its
Azure application uses a repository-specific GitHub OIDC federated credential,
so no client secret is stored in GitHub.

The environment defines these variables:

- `AZURE_CLIENT_ID`
- `AZURE_TENANT_ID`
- `AZURE_SUBSCRIPTION_ID`
- `AZURE_SIGNING_ENDPOINT`
- `AZURE_SIGNING_ACCOUNT`
- `AZURE_SIGNING_PROFILE`

It also contains the `UPDATE_SIGNING_KEY` secret. This is the base64-encoded
PKCS#8 Ed25519 private key paired with `updatePublicKeyHex` in `app/update.go`.
The release job uses it only after Authenticode signing to authenticate the
small update manifest. Never store the private key in the repository.

The Azure application needs the `Artifact Signing Certificate Profile Signer`
role on the signing account. The workflow itself requests only `id-token: write`
and `contents: write` in the protected publish job.

The expected Authenticode publisher is `CN=Brandon South`. Azure Artifact
Signing certificates are short-lived and renewed often, so check the subject
name rather than a thumbprint. Any change in signing identity must be noted in
`CHANGELOG.md` and `README.md`.

The optional `signing-check` phase builds, signs, and verifies the current
launcher and signed update metadata without creating or modifying a release.
It requires the matching source pin and includes `test-update.json` in the
artifact for verification. Use it after changing the OIDC or signing configuration.

## Prepare the guest

1. Add the technical changes to `CHANGELOG.md` and write the public announcement
   in `.github/release-notes/TAG.md`. Release notes use a one-line
   summary naming the version, then `# Features`,
   `# Fixes` and `## Dev` lists written for users, each item ending with its
   pull request or issue numbers, and a closing section thanking reporters or
   new contributors. Review the exact wording and links before publishing. The
   release workflow rejects missing or empty announcements instead of falling
   back to the changelog. Push both files to `master`.
2. Run the `Release` workflow with phase `prepare` and the new release tag.
3. The workflow applies the locked guest patches, runs the guest contract tests,
   rebuilds the image, builds the Try Omarchy importer (`try-omarchy-import.pyz`
   and its bootstrap `try-omarchy-import.sh`, stamped with the release tag),
   boots the instant account headlessly, authenticates every artifact, and
   creates a draft release.
4. Download the draft `SHA256SUMS`, add it under `app/testdata`, and update
   `defaultReleaseURL`, `defaultSumsSHA256`, and the embedded fixture name in
   `app/manifest.go`. Update `currentVersion` in `app/update.go` and the numeric and text versions
   in `app/versioninfo.rc` to the same tag. Regenerate
   `app/rsrc_windows_amd64.syso` using the commands in `versioninfo.rc`.
5. Run `scripts/release/validate-pin.py TAG`, commit, and push the pin.

The pinned tag must be on `master` before `publish` runs: the workflow's guard
requires `refs/heads/master`, and the publish phase verifies that the source pin
matches the draft release manifest. This has one consequence worth expecting:
CI also runs `validate-pin.py --require-public` on every push to `master`, so
while the release is still a draft the "Validate current release pin" step fails
on `master`. That failure is correct, because a fresh build from `master` genuinely
cannot download the pinned assets yet. It clears as soon as `publish` makes the
release public. Do not merge a pin bump to an unpublished release through a pull
request; land it as the deliberate pre-publish step and publish promptly.

## Test the draft on physical Windows

A GitHub draft's assets require authentication, while the launcher downloads
without a GitHub token. Do not point the launcher directly at the draft URL.
After the manifest pin is pushed:

1. Run the `signing-check` phase on the pinned commit and download its signed
   launcher artifact.
2. Download every draft asset into one folder with an authenticated browser or
   `gh release download TAG --dir candidate-assets`.
3. Serve that folder over loopback on the test PC. For example, from the asset
   folder run `py -m http.server 18080 --bind 127.0.0.1`.
4. Close Try Omarchy and copy `%LOCALAPPDATA%\TryOmarchy` to a separate test
   directory. Never use the only copy of a real guest for candidate testing.
5. Start the signed candidate with the copied data directory and the local
   payload:

   ```powershell
   .\TryOmarchy.exe `
     -dir C:\TryOmarchyCandidate `
     -release http://127.0.0.1:18080 `
     -sums-sha256 SHA256SUMS_DIGEST `
     -runtime-release http://127.0.0.1:18080 `
     -runtime-sums-sha256 SHA256SUMS_DIGEST `
     -no-update
   ```

Confirm that the existing desktop and files survive, the new external kernel
boots, reboot and poweroff work, and a second launch does not repeat the guest
compatibility repair. For the interruption check, quit normally before the first
candidate boot reports ready, then launch again. The copied install must retry the candidate
without reverting or downloading it again. For the rollback check, terminate
QEMU unexpectedly before readiness and launch again. The copied install must
restore its previous guest and runtime without downloading the failed payload.

Keep the release as a draft until the GPU, idle CPU, audio, input, resize, and
fullscreen checks in `docs/RUNTIME-VALIDATION.md` pass on physical hardware,
and every CHANGELOG entry's user check in `docs/RELEASE-ACCEPTANCE.md` passes.

The guest builder base is fixed in `guest-build/source.lock.json`. The runtime
build inputs are fixed in `runtime-build/sources.lock.json`, and the Runtime
workflow produces matching portable and source archives with licenses,
provenance, and per-file hashes. Both archives used by a production release
stay fixed in `guest-build/runtime.lock.json`; update that lock only after the
new runtime passes `docs/RUNTIME-VALIDATION.md`.

## Publish

Run the `Release` workflow again with phase `publish` and the same tag. It:

- verifies that the source pin exactly matches the draft manifest;
- runs launcher tests and produces the optimized Windows build;
- signs through Azure Artifact Signing using GitHub OIDC;
- signs current update metadata with the protected Ed25519 update key;
- verifies Authenticode before upload;
- publishes without changing `Latest`;
- verifies the public tagged launcher, checksum, manifest, and guest URL through
  both the current repository and the original repository redirect;
- marks the release `Latest`, then verifies both repositories' `latest/download`
  paths, including the legacy and current signed update feeds.

If public verification fails, the release stays published but does not replace
the previous `Latest` release.

The updater accepts only a correctly signed manifest, a newer supported version,
the expected repository release URL, and matching SHA256 values. Installed users
boot their receipt-pinned guest and runtime before any network check. After userspace reports ready, a cancellable background job checks the
signed feed and downloads to a stable resumable cache. The next launch
reauthenticates the signed metadata and cached files before applying them.
Restart to update in the tray requests a graceful shutdown; an update never
restarts a running guest on its own. Disabled automatic updates, explicit
payload pins and checkpoint recovery retain their existing meanings.

A launcher also completes its own embedded guest and runtime pins, independently
of the feed. For a v0.9.0 user updating to v0.10, the released launcher first
performs its existing launcher update hop. The v0.10 launcher's first start
boots the installed v0.9.0 guest and runtime quickly. Userspace readiness commits
the pending launcher hop, then the v0.10 payload downloads in the background.
The tray offers "Restart to update" once that payload is verified and staged.
The next start applies it locally, and readiness commits the guest and runtime
transactions. An already staged matching payload can be applied immediately.

Turning automatic updates off, including `-no-update`, stops feed checks for
newer releases; it still completes the installed launcher's own payloads.
Explicit command-line payload pins, checkpoint recovery, a failed-version
rollback, and offline portable mode skip this convergence. Both hops retain
their prior files until readiness, and an interrupted first boot retries while
an unexpected failure restores the previous payloads.

It stages the launcher and payload directories atomically. Launcher, runtime,
and guest updates stay rollback-capable until the guest's userspace readiness service
reaches the launcher after networking starts, so QMP responding during a
kernel panic cannot commit a bad update. The writable disk is
preserved, and the updated initramfs installs the small matching launcher
integration onto disks created by older releases without replacing their OS or
user data.

## Moving from preview to a normal version

Ship and test a bridge preview containing the normal-version updater before
publishing the first normal version. Set the release environment variable
`LEGACY_UPDATE_BRIDGE_TAG` to that published preview tag. Keep its launcher,
payload, and signed metadata available permanently. Publication of a normal
version refuses to proceed without verified bridge metadata.

Each release carries two signed feeds:

- `update-v2.json` describes the current release and is used by the bridge and
  all newer launchers.
- `update.json` is for older launchers, which only accept preview tags. Preview
  releases use their own metadata; normal releases carry the original signed
  bridge metadata unchanged.

An old installation that misses the bridge still finds it through the normal
release's legacy feed. After installing the bridge, its next scheduled update
check uses the current feed and can install the normal version. The Latest launcher
link always serves the current executable. Signature verification, pinned
payload hashes, and rollback remain required at both steps.

Before publishing the first normal version, test an old preview against the
public bridge feed and verify that it installs the bridge on a copied guest
disk. Test the exact signed normal-version candidate against locally served,
hash-pinned draft assets, including direct install, preservation, and forced
rollback. The updater intentionally accepts only GitHub release URLs, so the
bridge-to-normal-version update cannot fetch a private draft through a local
test URL. Immediately after publication, verify that update and both public
feeds on physical Windows. Automated tests cover feed routing and recovery-state
parsing, but do not replace these Windows checks.

Normal-version installations never automatically switch to a preview. The
workflow also prevents a later preview or older version from replacing a newer
normal Latest release. Retain the bridge feed on every future normal release so
infrequently used installations can still update.

## Following Omarchy releases

The `Follow Omarchy releases` workflow checks upstream's latest published stable
release daily at 07:37 UTC and can be started manually. When that release is
newer than the guest's pinned version, it exports the patched builder and adds
one numbered guest patch that pins the release's commit, tree, normalized
digest and literal version file, and resets `packageRelease` to 1. Existing
backports and authenticity checks stay in place, so an upstream change that
needs compatibility work fails validation instead of dropping a Windows fix.

The workflow refreshes the package transaction lock, pushes
`omarchy-release/vX.Y.Z` and opens a draft PR. A PR created with
`GITHUB_TOKEN` does not trigger pull-request workflows, so it dispatches CI with
`build_guest=true` and the Shell canary for the same commit. Only when both
complete runs pass for the unchanged PR head, including the guest image build
and boot, does it mark the PR ready for review. It never merges: a maintainer
reviews the upstream changes and new migrations, then merges. A failed lock
refresh or any failed, skipped or cancelled job leaves the draft for manual
work. Windows hardware acceptance, the existing-disk upgrade check and signed
release publication stay manual; merging does not update published downloads
or existing VMs.

Bot PR creation needs the Actions setting that allows it. If creation is
denied, the workflow keeps the pushed branch and reports a compare link. Forks
do not run the job. An existing open or closed PR, or a leftover branch, for a
release is never overwritten or recreated; after repairing it, dispatch CI with
`build_guest=true` and merge it through the normal review process.
