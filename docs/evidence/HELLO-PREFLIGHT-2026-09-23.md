# Windows Hello laptop preflight: September 23, 2026

The signed-in Windows 11 laptop initially reported `DeviceNotPresent` from
`UserConsentVerifier.CheckAvailabilityAsync`. After the owner set a Windows
Hello PIN, the same interactive scheduled task returned `Available`. The TPM
was present and ready. No PIN or other secret was sent to the guest or recorded.

A separate interactive test called `RequestVerificationAsync` and received
`Verified` after the owner approved the prompt. The owner described this
hidden-PowerShell test prompt as awkward but readable. This proves the enrolled
host can approve a Windows Hello request; it does not prove the guest sudo
integration or its final presentation.

`KeyCredentialManager.IsSupportedAsync` returned `True` in the same signed-in
session. The host projection exposes `RequestCreateForWindowAsync` and
`RequestSignForWindowAsync`, so implementation can bind key enrollment and
signing prompts to an owned desktop window. A hidden scheduled-task call to
`RequestCreateAsync` put Windows Security behind the launcher's startup window
and timed out. A follow-up call with a visible owner window put the Windows
Security PIN dialog above Omarchy, with the field focused, but no approval
arrived before its 90-second timeout. A later owned-window attempt did complete
key creation (`Success`), but PowerShell 5.1 could not convert the returned
WinRT public-key buffer. Its cleanup requested key deletion, and a follow-up
`OpenAsync` returned `NotFound`. After a fix for that test-harness conversion,
another enrollment attempt timed out without approval. Its stale broker was
closed and `OpenAsync` again returned `NotFound`.

With the owner watching, the next owned-window test completed key creation and
challenge signing (`Success` for each). The extracted SubjectPublicKeyInfo key
was 294 bytes and represented RSA-2048; the signature was 256 bytes. OpenSSL
verified the signature over the fresh 32-byte challenge with SHA-256 and RSA
PKCS#1 v1.5. RSA-PSS verification failed on this laptop, despite the current
[Microsoft KeyCredentialManager remarks](https://learn.microsoft.com/en-us/uwp/api/windows.security.credentials.keycredentialmanager?view=winrt-26100)
describing PSS. The owner entered the PIN only once during the create-then-sign
sequence. Signing must therefore not be treated as proof that a separate prompt
appeared for that specific request. The test key was deleted and `OpenAsync`
returned `NotFound`.

A subsequent experiment added an explicit `UserConsentVerifier` call before
signing, but stopped at its first key-creation prompt after 90 seconds without
approval. Its stale dialog was closed and `OpenAsync` returned `NotFound`.
Explicit per-request consent and signed guest sudo still need end-to-end tests.

A native C++/WinRT helper then compiled on the Windows CI runner in
[run 35942384825](https://github.com/omacom/try-omarchy-windows/actions/runs/35942384825).
The unsigned test executable had SHA-256
`92e98d3622652add218a326b92f1346eace75761f4b933eb87cd44763e0ebfc6`
locally and on the laptop. Its interactive, on-demand enrollment attempt timed
out after 90 seconds. A desktop capture showed the Windows Security PIN dialog
open over a black console window, but the test did not receive approval. The
signing stage never ran. The stale dialog was closed, and a separate
`OpenAsync` check returned `NotFound` for the disposable guest key. This run
does not validate fresh consent or native-helper signing. The test harness's
console presentation also needs correction before another acceptance pass.

The owner requested a rerun. The harness used `CreateNoWindow` for its console
process, then the native helper completed enrollment, explicit consent, signing,
and deletion (`exit=0` at each stage). The owner reported three Windows Security
prompts and entered the PIN three times: once for enrollment, once for consent,
and once for signing. The 294-byte public key was identical for both signed
payloads; each signature was 256 bytes and verified with OpenSSL using RSA
SHA-256 PKCS#1 v1.5. A follow-up `OpenAsync` returned `NotFound`. This proves
the native helper's separate consent check ran before signing on the laptop.
It also exposes a poor approval flow: the current helper asks for two PIN
entries per sudo request. Guest pairing and sudo approval or denial have not
been exercised yet.

Next acceptance for [#165](https://github.com/omacom/try-omarchy-windows/issues/165):
pair a per-guest public key after guest-password authorization, require a
fresh challenge and a signed response for an interactive `sudo` request,
verify it in the guest, then test one approval and one denial on this laptop.
Password fallback must remain available.
