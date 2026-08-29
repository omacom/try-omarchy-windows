# Handoff — laptop session: validate the one-click exe, sign it, ship

The release is HELD until this pass is done. Since the last handoff the app
grew full self-setup (commit 7ae4d48): first run now switches on WHP itself
(UAC prompt + the one Windows restart), downloads a portable WINQ-EMU runtime
(46 MB zip on the release, SHA256-verified) when no QEMU is installed, then
pulls the image. bootstrap.ps1 is no longer required for users. The public
story becomes: download TryOmarchy.exe, open it, done.

## State right now

- **Unsigned self-setup TryOmarchy.exe** (7.76 MB, master `7ae4d48`, Go 1.27)
  is on v0.0.3-preview with `TryOmarchy.exe.sha256`. SHA256:
  `dd1c64ca82a692097d3c03e3cf95cdd74e046a032bccde3289f2ab8f71d5ed48`
- `winq-emu-alpha10-portable.zip` (46 MB) is on the release and in SHA256SUMS:
  cmspam's Alpha 10 payload, same `bin/` layout as C:\WINQ-EMU, minus the
  console exe/qemu-img/installer. The app downloads it to
  `%LOCALAPPDATA%\TryOmarchy\runtime` only when it finds no QEMU.
- Lookup order: `C:\WINQ-EMU` → `%LOCALAPPDATA%\TryOmarchy\runtime` → stock
  `C:\Program Files\qemu` (CPU mode). Existing setups behave exactly as before.
- New fallback: a startup-dead QEMU in GPU mode retries with CPU args on the
  same binary (after the existing no-audio retry).
- The live site still shows the old bootstrap flow ON PURPOSE - it stays true
  until the signed exe is up. The new copy is pre-staged on `one-click`
  branches (this repo: README; tryomarchy-site: index.html). Merge them at the
  end, don't retype.
- Cert ready: Trusted Signing `southforgesigning` / profile `conduit`
  (PublicTrust, Active). docs/SIGNING.md has the details; sign.ps1 defaults
  are correct.

## Test pass (in this order)

1. `git pull`. `gh auth status` = tsouth89. Get the release exe:

   ```powershell
   irm https://tryomarchy.com/download -OutFile TryOmarchy.exe
   Get-FileHash .\TryOmarchy.exe   # DD1C64CA...D5ED48
   ```

2. **Regression (5 min)**: double-click it on the laptop as-is. C:\WINQ-EMU
   must win: no new prompts, no downloads, straight to the desktop, GPU mode
   in `%LOCALAPPDATA%\TryOmarchy\vm\shell.log`.

3. **Runtime download (10 min)**: `.\TryOmarchy.exe -winq C:\nope` - forces
   the C:\WINQ-EMU miss. Expect "Downloading the graphics engine..." then
   "Unpacking...", then a normal GPU boot from
   `%LOCALAPPDATA%\TryOmarchy\runtime`. Check video/audio still good
   (same QEMU build, new location). shell.log says which binary ran.

4. **Bare-machine flow (the money test, two restarts, ~20 min)**. This is
   what every real user hits, so run it once even though it's tedious:

   ```powershell
   # elevated - make the laptop look factory-fresh
   Rename-Item C:\WINQ-EMU C:\WINQ-EMU.off
   Rename-Item 'C:\Program Files\qemu' 'C:\Program Files\qemu.off'
   Remove-Item -Recurse %LOCALAPPDATA%\TryOmarchy\runtime -ErrorAction SilentlyContinue
   Remove-Item %LOCALAPPDATA%\TryOmarchy\whp-requested -ErrorAction SilentlyContinue
   dism /online /disable-feature /featurename:HypervisorPlatform /norestart
   shutdown /r /t 0
   ```

   After the restart, double-click the exe and walk it like a stranger:
   - info box explaining virtualization → OK → UAC prompt → "Switching on
     Windows' virtualization..." window → restart prompt → Yes
   - after the restart, double-click again → graphics engine download →
     desktop (image already present, so no 1.4 GB wait)
   - then restore: rename both directories back. WHP is re-enabled by the
     test itself. (This also re-checks WSL2 still works if you use it.)

5. **Sign** (one-time prereqs in docs/SIGNING.md if missing):

   ```powershell
   winget install Microsoft.Azure.TrustedSigningClientTools
   winget install Microsoft.WindowsSDK.SignTool
   az login --tenant b87fd204-c1aa-47fb-a84c-2e89f6ec5073   # tsouth2@gmail.com
   powershell -ExecutionPolicy Bypass -File scripts\sign.ps1 -Path .\TryOmarchy.exe
   ```

   Sign the exact binary you just validated. Smoke it once more signed
   (Properties → Digital Signatures → Brandon South), then upload:

   ```powershell
   "$((Get-FileHash .\TryOmarchy.exe).Hash.ToLower())  TryOmarchy.exe" |
       Set-Content TryOmarchy.exe.sha256 -Encoding ascii
   gh release upload v0.0.3-preview TryOmarchy.exe TryOmarchy.exe.sha256 --clobber
   ```

   Verify: `irm https://tryomarchy.com/download -OutFile check.exe` → hash
   matches, signature shows.

## Flip the copy (only after the signed asset is up)

Everything is pre-written on `one-click` branches:

```powershell
# this repo
git checkout master; git merge one-click; git push
# site repo (push = deploy)
cd ..\tryomarchy-site
git checkout main; git merge one-click; git push
```

Then the release notes: `gh release edit v0.0.3-preview` and replace the
first paragraph with:

> **Download: [TryOmarchy.exe](https://github.com/tsouth89/try-omarchy-windows/releases/download/v0.0.3-preview/TryOmarchy.exe)** (8 MB, signed). Open it - first run switches on Windows' virtualization (one permission prompt, one restart), downloads the GPU runtime and the Omarchy image, and boots you into setup. Checksum in TryOmarchy.exe.sha256.

Finally tick the NOTES.md road items (sign + one-click) and announce.

## Known limits to keep in mind (not blockers)

- Hardware virtualization off in firmware is a hard floor. The app detects
  it (feature enabled + rebooted + hypervisor still absent) and names the
  BIOS setting instead of looping. Windows 11-era machines ship with it on.
- SmartScreen reputation builds over downloads even for signed exes - a
  soft "protected your PC" prompt can still appear for the first users.
  Publisher shows Brandon South either way.
- Post-release (genuinely minor): image v3 papercuts (narrow-screen
  screensaver font, spare gettys, socat patch), CI signing via
  azure/trusted-signing-action.
- Announcement numbers, verified in NOTES/FINDINGS: 6.08s to
  graphical.target on the Ryzen 5 5625U laptop, 66 s image download, virgl +
  Venus Vulkan, Omarchy 4.0.1, 22 themes + screensavers, two-way clipboard,
  folder sharing, 8 MB app, ~1.5 GB total first-run download, Windows 11
  Home & Pro, MIT, disk untouched.
