# 1Password Windows Hello unlock on the laptop, September 26, 2026

The AMD Windows 11 laptop ran a launcher built from the 1Password branch
(SHA-256 `af06b287deddf72f02ec6175e649952bc777683ce833adfcf565543b3528ca45`)
with a guest image built from the same patches (through `0099`, compatibility
revision 37). It upgraded the disposable v0.5.0 `fresh` test install, which
then had revision 37, the Windows Hello broker, the 1Password agent and its
password dialog. 1Password 8.12.36 was installed from the Omarchy repository
and started in the background without signing in.

The owner ran each step in an Omarchy terminal. `pkcheck` requested polkit
actions for the running 1Password main process, which is what 1Password's own
unlock button does.

| Step | Result | Launcher log |
| --- | --- | --- |
| `sudo try-omarchy-windows-hello enable` | Passkey dialog and one PIN; paired | `enroll approved` |
| `sudo try-omarchy-windows-hello onepassword enable` | Agent unit enabled and started | none |
| `pkcheck` for `com.1password.1Password.unlock` | One Windows Hello PIN; `AUTHORIZED` | `onepassword-unlock approved` |
| Same, Hello prompt canceled | Guest password dialog; `AUTHORIZED` after the password | `onepassword-unlock denied: canceled` |
| `pkcheck` for `com.1password.1Password.authorizeCLI`, twice | Password dialog each time with no Hello prompt; authorized once with the password, dismissed once on cancel | none |
| `sudo try-omarchy-windows-hello disable` | Agent unit disabled and stopped, sudo rule removed, passkey deleted | `disable approved` |

Afterwards 1Password was removed, the guest's idle setting restored and the
guest powered off. Signing in to a real 1Password account and using its
fingerprint button was not part of this run.
