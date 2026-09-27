# Windows Hello for guest sudo

This integration is opt-in. Ordinary guest password authentication remains the
default and the fallback whenever Hello is denied, canceled, unavailable, or
disconnected. It applies only to interactive `sudo` PAM requests in the Omarchy
guest. It does not change login, screen unlock, SSH, or Windows authentication.

## Turning it on

In an Omarchy terminal:

```bash
sudo try-omarchy-windows-hello enable
```

`sudo` asks for the guest password first. Instant trial accounts have
passwordless sudo, so Hello only matters once the account has a password. Windows then asks to create a passkey
for "Try Omarchy" and for Windows Hello once. After that, each `sudo` in that
guest shows one Windows Hello prompt; cancel it to type the password instead.
`sudo try-omarchy-windows-hello disable` removes the PAM rule, forgets the
pairing and deletes the Windows passkey.

## Unlocking 1Password

If you install 1Password in Omarchy, it can use the same pairing:

```bash
sudo try-omarchy-windows-hello onepassword enable
```

Then in 1Password, turn on **Settings > Security > Unlock using system
authentication**, and lock and unlock once with your account password.
Afterwards the fingerprint button in 1Password shows one Windows Hello prompt.
Canceling it opens a password dialog for your guest password instead.
1Password still asks for its account password after it restarts and whenever
its own password interval expires. `sudo try-omarchy-windows-hello onepassword
disable` turns this off, and disabling Windows Hello also stops it.

A root-only polkit agent (ported from Try Omarchy for macOS) registers only for
the installed `/opt/1Password/1password` app of that user, and only while the
user's local desktop session is active. Only the
`com.1password.1Password.unlock` action asks Windows Hello; CLI and SSH agent
requests, and any other process, use the normal password dialog. The agent
answers polkit only after the broker verifies a signed `onepassword-unlock`
approval, whose operation and `1password` service are part of the signed client
data, so a sudo approval can never unlock 1Password. The password dialog runs as
the user through the standard polkit PAM session, and its exit status alone
never authorizes anything.

The [laptop run](evidence/ONEPASSWORD-HELLO-LAPTOP-2026-09-26.md) covered the
unlock with Windows Hello, the password fallback after a cancel, and a CLI
request that never reached Windows Hello.

## How it works

Windows Hello answers as a WebAuthn platform authenticator. The launcher calls
`webauthn.dll` directly; there is no separate helper program. The relying party
ID is `try-omarchy.invalid`, a name that can never belong to a website.

**Enrollment.** The guest broker creates a random 256-bit guest ID and sends an
`enroll` request over the root-only virtio port `dev.tryomarchy.authentication`.
The launcher calls `WebAuthNAuthenticatorMakeCredential` for a platform ES256
credential with user verification required, and returns the authenticator
data. The guest checks the relying party hash and the user-present,
user-verified and attested-credential flags, then pins the credential ID and its
P-256 public key in `/var/lib/try-omarchy/windows-hello/enrollment.json`
(root-only). Only after that does `try-omarchy-windows-hello` add its single
`auth sufficient pam_exec.so` rule to `/etc/pam.d/sudo`.

**Each sudo.** The broker sends a `sudo` request with a fresh 256-bit challenge,
a request ID, the guest ID, the PAM user, requesting user, TTY and the pinned
credential ID. The launcher refuses unless its own Omarchy window is in front,
allows one prompt at a time and waits 3 seconds after a denial before prompting
again. It calls `WebAuthNAuthenticatorGetAssertion` for that one credential with
user verification required. The signed client data is a fixed JSON rendering of
every request field (`helloClientData` in `app/hello_protocol.go`, `client_data`
in the broker); both sides build the same bytes. The guest verifies the ES256
signature over the authenticator data and SHA-256 of those bytes with the
pinned key, and checks the relying party hash and both flags before PAM returns
success. An unsigned approval flag is never sufficient, and PIN, password and
biometric material stay in Windows.

**Disable.** The launcher lists the platform credentials Windows holds for
`try-omarchy.invalid` and deletes the guest's credential only if it appears in
that list, so a guest can never remove any other passkey. On Windows builds
without the list API, the guest still disables itself and tells the user to
remove the passkey in Settings > Accounts > Passkeys.

Requests and responses have strict field sets, a 4 KiB limit and one response
per request. Malformed requests close the port without a reply. The launcher
logs only the operation and outcome; the guest logs only that it fell back to
the password.

## History

The first design used a `KeyCredential` plus a separate
`UserConsentVerifier` check, because a `KeyCredential` signature alone did not
prove fresh consent on the test laptop. That cost two PIN prompts per sudo
request. The [WebAuthn preflight](evidence/HELLO-WEBAUTHN-PREFLIGHT-2026-09-26.md)
showed one prompt per signature with the user-verified flag set, so the bridge
moved to WebAuthn. The earlier [preflight](evidence/HELLO-PREFLIGHT-2026-09-23.md)
records the KeyCredential results.

## Acceptance

The [laptop run](evidence/HELLO-SUDO-LAPTOP-2026-09-26.md) covered pairing,
one approval, one cancel, a request while another window was in front, and
disable. The list below is what that run and the tests check.

- Protocol tests cover malformed fields, spoofed user and TTY, wrong credential,
  foreign relying party, missing user verification, changed client data and
  denial. See `app/hello_protocol_test.go` and the guest's
  `test_windows_hello_broker.py`.
- A real Windows 11 laptop pairs a guest, approves one guest `sudo`, and cancels
  one, which falls through to the password prompt.
- Each sudo request shows exactly one Windows Hello prompt, above the Omarchy
  window, and the prompt does not appear while another window is in front.
- Disabling removes the PAM rule and the Windows passkey without disturbing
  other PAM policy or other passkeys. Unsupported or unenrolled hosts keep
  password authentication.

Microsoft documents the [WebAuthn platform API](https://learn.microsoft.com/en-us/windows/win32/api/webauthn/)
and its [header](https://github.com/microsoft/webauthn).
