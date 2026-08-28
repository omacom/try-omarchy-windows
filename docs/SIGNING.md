# Code signing (Azure Trusted Signing)

Set up 2026-08-28. Signing runs on Windows via `scripts/sign.ps1`; everything
below is the state of the Azure side and the one-time Windows prereqs.

## The live setup

- Trusted Signing account: `southforgesigning` (resource group
  `southforge-signing`, subscription "Azure subscription 1", tenant
  b87fd204-c1aa-47fb-a84c-2e89f6ec5073, sign in as tsouth2@gmail.com —
  `az login --tenant b87fd204-...`)
- Endpoint: `https://eus.codesigning.azure.net`
- Certificate profile: **`conduit`** (PublicTrust, Active). Shared across
  Brandon's products: the account's Basic SKU allows exactly one PublicTrust
  profile, and the profile name is invisible to end users anyway. A dedicated
  `try-omarchy` profile needs the Premium SKU (~10x cost) — not worth it.
- Publisher shown to users: **Brandon South** (the completed individual
  identity validation). This CANNOT be changed per-profile; showing a company
  name (e.g. Southbound Software) would need a new organization identity
  validation of a real registered entity — parked deliberately.
- Certs are short-lived (3 days, rotated daily by the service) — that's normal
  for Trusted Signing; the timestamp (`http://timestamp.acs.microsoft.com`)
  keeps signatures valid forever.
- Roles: tsouth2@gmail.com and the conduit CI service principal both hold
  "Artifact Signing Certificate Profile Signer" on the account. Signing is
  authorized by that role; there is no certificate file or secret to manage.

## Signing on the laptop

One-time:

```powershell
winget install Microsoft.Azure.TrustedSigningClientTools
winget install Microsoft.WindowsSDK.SignTool   # any recent Windows SDK signtool works
az login --tenant b87fd204-c1aa-47fb-a84c-2e89f6ec5073
```

Then, per artifact:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\sign.ps1 -Path .\TryOmarchy-Setup.exe
```

The script finds the dlib and signtool, signs with SHA256 + the ACS timestamp
server, and verifies. Defaults already point at the endpoint/account/profile
above.

## What gets signed

The app shell exe and the installer. NOT the guest image artifacts
(rootfs/kernel/initramfs) — they're data, integrity-checked by SHA256SUMS,
and signing 1.4 GB would be pointless.

## CI later

`azure/trusted-signing-action` signs in GitHub Actions using a federated
credential or the existing service principal (35f2e38f-... already holds the
signer role). Wire it into the release workflow once the native app builds in
CI — then releases are signed without a human at a keyboard.
