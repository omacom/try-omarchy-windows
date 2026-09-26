# Windows Hello WebAuthn preflight, September 26, 2026

The AMD Windows 11 laptop ran `TryOmarchyWebAuthnPreflight.exe` (CI build,
SHA-256 `96d60b694e37766deccb928eb2c1ff8d1ae3d3d1c5d77fa2cfd5286f71b503c7`) as
the signed-in owner. It created one platform credential for the placeholder
relying party `preflight.try-omarchy.invalid`, requested two assertions with
user verification required, then deleted the credential.

- `webauthn.dll` reported API version 9 and a user-verifying platform
  authenticator.
- Windows showed one dialog to create the passkey, then one PIN prompt for
  creation and one for each assertion: three PIN entries in total. Each
  assertion took about 3.5 seconds including the PIN.
- The credential was ES256 with a 32-byte credential ID. Both assertions had the
  user-present and user-verified flags set and sign counts 1 and 2. OpenSSL
  verified both signatures over the authenticator data and the SHA-256 of the
  exact client data.
- `WebAuthNDeletePlatformCredential` returned success.

The owner also confirmed the physical result. This established that WebAuthn
gives one Windows Hello prompt per signed sudo request, where the earlier
KeyCredential design needed two. The preflight program was removed once the
launcher called `webauthn.dll` directly.
