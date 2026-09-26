# Windows Hello for guest sudo

This integration is opt-in. Ordinary guest password authentication remains the
default and the fallback whenever Hello is denied, canceled, unavailable, or
disconnected. It applies only to interactive `sudo` PAM requests in the Omarchy
guest. It does not change login, screen unlock, SSH, or Windows authentication.

## Pairing

The guest installs a root-only authentication virtio port and helper, but does
not change PAM until the user enables Hello in **Setup → Security**. Enabling
first requires the guest password through normal sudo. The guest then creates
a random 256-bit guest ID and a fresh enrollment challenge. The signed Windows
helper creates a named, per-guest Windows Hello `KeyCredential`, returns its
RSA public key in SubjectPublicKeyInfo DER, and signs the enrollment challenge.
The guest verifies the signature before it pins that public key and atomically
adds its exact `auth sufficient` PAM rule. Disabling removes the rule before it
deletes pairing state and requests host-key deletion. A lost host key or PIN
reset leaves password authentication usable; re-pairing requires the guest
password again.

The Windows helper owns a small visible window for enrollment and approval.
It uses `RequestCreateForWindowAsync` and `RequestSignForWindowAsync` with that
window ID. For each sudo request it first calls the desktop
`UserConsentVerifierInterop.RequestVerificationForWindowAsync` with a message
identifying the action and requires `Verified` before signing. A key-creation
approval was reused by a subsequent sign call without a second PIN prompt on
the test laptop, so the signing result alone does not establish fresh user
consent. A hidden task put a Windows Security dialog behind the launcher on
the [test laptop](evidence/HELLO-PREFLIGHT-2026-09-23.md), so enrollment cannot
use an unowned prompt. The prompt message and owner window identify Try Omarchy
and the requested sudo action. The app cannot restyle the Windows Security
dialog itself.

In a later native-helper preflight, the explicit consent check and key signing
each required a PIN entry. That produces two PIN prompts for one sudo request.
Resolve this presentation and approval flow before enabling guest sudo pairing
for users.

The native [WebAuthn assertion API](https://learn.microsoft.com/en-us/windows/win32/api/webauthn/nf-webauthn-webauthnauthenticatorgetassertion)
is a candidate for combining user verification and a signed challenge in one
operation. Its request options include a user verification requirement, and its
assertion carries authenticator data and a signature. This is a design lead,
not an accepted replacement: it needs a disposable laptop test and a revised
guest verifier before use.

## Approval

The root-only guest helper accepts only a `sudo` PAM service with a valid
interactive TTY and local account names. Each request has a random 256-bit
challenge and unique request ID. Requests and responses have strict field sets,
a size limit, one-response semantics, and a short timeout. The host permits
one prompt at a time, checks that its own QEMU process is still running and its
window is frontmost, and refuses an unknown guest ID or signing key.

The Windows Hello key signs canonical bytes containing the guest ID, request
ID, challenge, PAM user, requesting user, `sudo` service, TTY, issue and expiry
times, and key ID. The approval expires after 15 seconds. The guest checks all
fields against its pending request, checks the clock window, requires the pinned
public key, and verifies its enrolled RSA SHA-256 signature before PAM can return
success. An unsigned approval flag is never sufficient. PIN, password, and
biometric material stay in Windows.

The guest determines the valid RSA signature padding during enrollment and
pins it with the public key. The test laptop produced a PKCS#1 v1.5 signature;
Microsoft's current KeyCredentialManager remarks describe PSS. A later approval
must verify with the pinned format. The host must fail closed if it cannot
identify a supported format.

The Windows helper and port are available only for the current interactive
session. The guest rejects stale replies after a VM restart, duplicate replies,
concurrent requests beyond the bound, and any response to another challenge.
The host logs outcome codes without request secrets or credential data. The
guest logs only a reason for falling through to password.

## Acceptance

- Protocol tests cover malformed fields, spoofed user and TTY, key mismatch,
  replay, expiry, denial, cancellation, timeout, port disconnect, and VM
  restart.
- A real Windows 11 laptop approves one guest `sudo` request and denies one;
  denial falls through to the normal guest password prompt.
- Each sudo request shows a separate consent prompt, including a request soon
  after enrollment or another approval.
- The owner window and Windows Security prompt are visible above QEMU, have
  clear Try Omarchy wording, and close after completion.
- Disabling removes the managed PAM rule without disturbing other PAM policy.
  Unsupported or unenrolled hosts keep password authentication.

Microsoft documents [Windows Hello key creation and challenge signing](https://learn.microsoft.com/en-us/windows/apps/develop/security/windows-hello-auth-service),
the [KeyCredentialManager API](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.keycredentialmanager?view=winrt-26100),
the [window-bound signing API](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.keycredential.requestsignforwindowasync),
the [desktop consent API](https://learn.microsoft.com/en-us/windows/win32/api/userconsentverifierinterop/nf-userconsentverifierinterop-iuserconsentverifierinterop-requestverificationforwindowasync),
and the [desktop window-owner requirement](https://learn.microsoft.com/en-us/windows/apps/develop/ui/display-ui-objects).
