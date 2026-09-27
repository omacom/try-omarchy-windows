# Windows Hello sudo on the laptop, September 26, 2026

The AMD Windows 11 laptop ran a launcher built from the Windows Hello branch
(SHA-256 `e6483135ef3e9094be1e790aaed482cbee22d2b05b8c11d0731a8dab1f67bab0`)
with a guest image built from the same branch (guest patch 0098, compatibility
revision 36; `SHA256SUMS` digest
`095383f38ed2efd80d08df163a2b72bd1d08d4f5c84db5f016013fcb2b28cdcb`). It
upgraded the disposable `fresh` v0.4.0 test install. After boot the guest had
revision 36, the broker and command, and `dev.tryomarchy.authentication` as a
root-only (0600) port. No PAM rule or pairing state existed.

The instant trial account has passwordless sudo, so for this test its
`00-try-omarchy-trial` rule was set aside and a temporary password-required
sudo rule added. Both were restored afterwards. The owner ran every step in an
Omarchy terminal:

| Step | Result | Launcher log |
| --- | --- | --- |
| `sudo try-omarchy-windows-hello enable` | Guest password, then the Windows "create a passkey" dialog and one PIN; enabled | `enroll approved` |
| `sudo -k; sudo whoami` | One Windows Hello PIN prompt; `root` without the guest password | `sudo approved` |
| Same, prompt canceled | Guest password prompt appeared; `root` after the password | `sudo denied: canceled` |
| Same, another Windows window in front | No Hello prompt; password prompt in the guest | `sudo denied: Omarchy window is not in front` |
| `sudo try-omarchy-windows-hello disable` | Disabled; the Try Omarchy passkey was gone from Windows | `disable approved` |

Afterwards `/etc/pam.d/sudo` had no Windows Hello rule. The guest powered off
cleanly.
