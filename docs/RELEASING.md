# Releasing Try Omarchy

Releases use a two-phase GitHub Actions workflow so the signed launcher can pin
the guest manifest produced for that same release without rewriting source code
inside CI.

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

The Azure application needs the `Artifact Signing Certificate Profile Signer`
role on the signing account. The workflow itself requests only `id-token: write`
and `contents: write` in the protected publish job.

The optional `signing-check` phase builds, signs, and verifies the current
launcher without creating or modifying a release. Use it after changing the
OIDC or signing configuration.

## Prepare the guest

1. Add the new version section to `CHANGELOG.md` and push it to `master`.
2. Run the `Release` workflow with phase `prepare` and the new preview tag.
3. The workflow applies the locked guest patches, runs the guest contract tests,
   rebuilds the image, boots the instant account headlessly, authenticates every
   artifact, and creates a draft release.
4. Download the draft `SHA256SUMS`, add it under `app/testdata`, and update
   `defaultReleaseURL`, `defaultSumsSHA256`, and the embedded fixture name in
   `app/manifest.go`.
5. Run `scripts/release/validate-pin.py TAG`, commit, and push the pin.

The guest builder base is fixed in `guest-build/source.lock.json`. The temporary
WINQ-EMU binary source and checksum are fixed in `guest-build/runtime.lock.json`
until the runtime is built from source in CI.

## Publish

Run the `Release` workflow again with phase `publish` and the same tag. It:

- verifies that the source pin exactly matches the draft manifest;
- runs launcher tests and produces the optimized Windows build;
- signs through Azure Artifact Signing using GitHub OIDC;
- verifies Authenticode before upload;
- publishes without changing `Latest`;
- verifies the public tagged launcher, checksum, manifest, and guest URL;
- marks the release `Latest`, then verifies the `latest/download` path.

If public verification fails, the release stays published but does not replace
the previous `Latest` release.
