# Handoff — laptop session: sign TryOmarchy.exe, ship v0.0.3 clean

Goal: the announcement goes out with a signed exe and nothing major pending.
Everything else is already live: tryomarchy.com (with /download and
/bootstrap.ps1 redirects), README, release notes, repo About. The only thing
between here and a clean release is signing, and signing only runs on Windows.

## State right now

- **Unsigned TryOmarchy.exe** (7.6 MB, built on the Linux box from master
  `88085a3`, Go 1.27, flags from the README) is attached to
  [v0.0.3-preview](https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.3-preview)
  together with `TryOmarchy.exe.sha256`. SHA256:
  `17b1e276090b8d2c22ad54c560a1d9a6a04fe4b0a9a9acf2b836b8315f853f28`
- `https://tryomarchy.com/download` 302s to
  `releases/latest/download/TryOmarchy.exe` — replacing the release asset is
  all it takes to swap in the signed build; the site needs no change for that.
- **The cert is ready.** Trusted Signing account `southforgesigning`, profile
  `conduit` (PublicTrust, **Active**), publisher shown: Brandon South. Details
  and prereqs in docs/SIGNING.md. `scripts/sign.ps1` already defaults to the
  live endpoint/account/profile. Ignore any older note saying the profile
  doesn't exist — SIGNING.md is current.
- **Caveat to test around:** the exe includes the splash/icon and
  first-run-progress-window fixes that landed AFTER the hardware-validated
  commit (81c19fa). Same code NOTES said to ship, but smoke test the signed
  exe before announcing.

## Steps (~20 min)

1. `git pull` this repo. `gh auth status` should say tsouth89.

2. One-time signing prereqs (skip what's already installed):

   ```powershell
   winget install Microsoft.Azure.TrustedSigningClientTools
   winget install Microsoft.WindowsSDK.SignTool
   az login --tenant b87fd204-c1aa-47fb-a84c-2e89f6ec5073   # tsouth2@gmail.com
   ```

3. Get the exe. Downloading the release asset is simplest (a laptop `go build`
   with a different Go version won't reproduce the hash):

   ```powershell
   irm https://tryomarchy.com/download -OutFile TryOmarchy.exe
   Get-FileHash .\TryOmarchy.exe   # must be 17B1E276...F853F28
   ```

4. Sign (the script signs SHA256 + ACS timestamp, then verifies):

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\sign.ps1 -Path .\TryOmarchy.exe
   ```

5. Smoke test the SIGNED exe. For the full first-run flow, move
   `%LOCALAPPDATA%\TryOmarchy\guest` aside first (or run with `-fresh` for a
   clean disk only). Check: progress window actually visible, ends in the
   setup form (or straight to desktop on the existing disk), title stays
   "Try Omarchy", splash + taskbar icon look right, poweroff from Omarchy
   exits the app. `%LOCALAPPDATA%\TryOmarchy\vm\shell.log` on weirdness.
   Properties → Digital Signatures should show Brandon South.

6. Replace the release assets — signing changed the file, so the checksum
   must be regenerated (keep the two-space sha256sum format):

   ```powershell
   "$((Get-FileHash .\TryOmarchy.exe).Hash.ToLower())  TryOmarchy.exe" |
       Set-Content TryOmarchy.exe.sha256 -Encoding ascii
   gh release upload v0.0.3-preview TryOmarchy.exe TryOmarchy.exe.sha256 --clobber
   ```

7. Verify the public path end to end:

   ```powershell
   irm https://tryomarchy.com/download -OutFile check.exe
   Get-FileHash .\check.exe        # matches the new sha256
   # Properties → Digital Signatures → Brandon South, timestamped
   ```

8. Flip the copy — three places still say the exe is unsigned. Do this only
   after the signed asset is up (any machine):

   - **tryomarchy-site/index.html**: delete the whole paragraph
     `<p class="note">The app isn't signed yet, so SmartScreen may warn you. ...</p>`
     (keep the download note above it). Commit + push = deploys.
   - **README.md**, Try it section: drop the sentence
     `The exe isn't signed yet, so SmartScreen may warn: "More info", then "Run anyway".`
   - **Release notes**: `gh release edit v0.0.3-preview` and drop
     `It is not signed yet, so SmartScreen may warn: "More info", then "Run anyway".`
   - **NOTES.md**: tick the "Sign the exe" road item.

## Genuinely post-release (nothing here blocks announcing)

- Image v3 papercuts: narrow-screen screensaver font (91 cols at 1366x768
  renders the compact logo), mask spare tty2-6 gettys, fold guest-build patch
  0003 into the shipped image.
- CI signing via azure/trusted-signing-action once the app builds in CI
  (the service principal already holds the signer role).
- Announcement numbers, all verified in NOTES/FINDINGS: 6.08s to
  graphical.target on the Ryzen 5 5625U laptop, 66 s first-run image
  download, virgl + Venus Vulkan, Omarchy 4.0.1, 22 themes + screensavers,
  two-way clipboard, folder sharing, 7.6 MB app, ~1.4 GB image, Windows 11
  Home & Pro, MIT, disk untouched.
