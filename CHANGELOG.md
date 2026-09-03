# Changelog

## v0.0.9-preview - 2026-09-02

### Features
- Updated new and reset guest images to Omarchy 4.0.2, a security release covering sshd hardening, browser policy directories, sudoers tightening, and signed packages from the Omarchy repository. Existing writable guests keep their installed OS and user data; the updated initramfs adds the matching launcher integration without replacing them.
- Added disk-space checks before the large guest download, unpack, and writable-disk copy, using sizes from the authenticated guest manifest, so a full drive is reported before the expensive step instead of after a partial copy.
- Added an experimental offline portable mode (`-portable`) that runs from a payload and data folder beside the launcher, keeps all guest state on the removable drive, makes no setup-time network requests, and uses a compact QCOW2 overlay that survives drive-letter changes and works on exFAT. See docs/PORTABLE_USB.md.
- Added loopback-only port forwarding into Omarchy (`-forward tcp:8080:80`) and an opt-in SSH preset (`-ssh 2222`) that starts sshd for that session and authorizes your public key for the Omarchy account. Nothing listens unless asked, and nothing is reachable from the network.
- The shared folder now appears inside Omarchy under its own name (`~/Work` for `C:\Users\me\Work`) as well as at `/mnt/host`. The link is removed again on launches that share nothing, and a real folder with content is never replaced.
- Added `try-omarchy-export` inside the guest: one archive with your configuration, theme, and added packages plus a restore script for a real Omarchy install. See docs/MIGRATION.md.
- Added a settings window (`-settings`) and a persistent settings file (`settings.json` in the data folder) for fullscreen, guest memory, the shared folder, port forwards, and the SSH key, with matching flags that win for a single launch. `-memory` is new.
- Added `-diagnostics`, which writes one zip of launcher and QEMU logs, guest console output, settings, update state, and machine facts for bug reports.

### Fixes
- Stopped setup on ARM64 Windows PCs with a clear explanation instead of failing through a WHP feature enable, a reboot, and impossible BIOS advice. Try Omarchy remains x86_64-only.
- Fixed startup on PCs whose hypervisor refuses nested virtualization, such as Intel Core Ultra laptops and machines with the full Hyper-V feature set. The launcher now retries with the interrupt controller in QEMU instead of failing, and the source-built runtime no longer treats the refusal as fatal.
- Shipped yay and the base-devel toolchain in new and reset guest images, so Omarchy's AUR install and update flows work out of the box.
- Fixed screen recording in new and reset guest images, which never started because the recorder was missing, and shipped the other tools Omarchy's keybindings and menus expect: the screenshot editor, OCR and QR capture, emoji and clipboard paste, man pages, the calculator, writer and video trimmer, Herdr, and the screen-share picker.
- Kept launcher and guest-image rollback active until the booted guest reaches userspace and networking. QEMU's control socket alone can answer during a kernel panic, so it is no longer treated as proof that an update is healthy.
- Preserved existing writable guests when a release raises the virtual disk size. The launcher now grows the disk in place instead of mistaking it for an incomplete first-run copy and replacing it with the factory image.
- Bound portable QCOW2 data to the authenticated factory-image digest so replacing its backing payload is refused instead of risking silent filesystem corruption.
- Rebuilt the Windows runtime to avoid 1 ms SDL redraw polling while the guest is idle. Real-hardware idle CPU and graphics checks still gate publication.
- Hardened failed-update recovery, settings and receipt writes, clipboard size checks, audio fallback, directory setup, and diagnostics redaction.

## v0.0.8-preview - 2026-08-30

- Fixed the launcher quitting on its own after about half an hour. Omarchy kept running, but the Windows key and every Windows shortcut went back to Windows, and the window could no longer be closed normally.
- Fixed setup blaming your connection when the real problem was a full disk.
- Removed a stall of about a minute when the bundled runtime rolled back to its previous version.
- Added resumable downloads so an interrupted setup continues where it stopped instead of fetching the payload again, with bounded retries when antivirus or indexing briefly locks a finished file.
- Added version details to the launcher, so Explorer, Task Manager and the Windows permission prompt now show Try Omarchy instead of a blank entry.
- Added a source-locked CI build for the patched Windows QEMU runtime, including matching source, licenses, package inventory, provenance, and per-file hashes.
- Added isolated signed test launchers so runtime candidates can be exercised without changing the production payload.
- Hardened runtime packaging and validated clean setup, CPU fallback, scoped Windows-key handling, clipboard sharing, shutdown, relaunch, and persistent guest data in a nested Windows VM.
- Made text clipboard sharing survive late guest startup, Wayland reconnects, early Windows copies, and temporary Windows clipboard contention.
- Updated the guest image to nautilus 50.3 and fd 10.5.
- Known limitation: Win+L still locks Windows instead of reaching Omarchy. Windows reserves that shortcut and no application can intercept it, so rebind the Omarchy action if you need it.

Thanks to [Tom Ballard](https://github.com/tcballard) for resumable downloads in [PR #7](https://github.com/tsouth89/try-omarchy-windows/pull/7), and to everyone who reported Windows shortcuts leaking through while Omarchy was running.

## v0.0.7-preview - 2026-08-30

- Added CI for launcher builds, release-pin validation, and guest patch contracts.
- Added a two-phase release workflow that rebuilds and smoke-tests the guest, signs the optimized launcher through Azure OIDC, and verifies public downloads before marking a release Latest.
- Added authenticated automatic updates for the launcher, bundled runtime, and factory guest image, with staged installs and automatic rollback after a failed first boot.
- Added bounded retries for temporary DNS, connection, rate-limit, and server failures during setup downloads.
- Made instant-mode credentials explicit in the account choice, setup splash, and a one-time first-desktop notification.
- Removed the duplicate Windows pointer over the guest-rendered cursor, with `-host-cursor` retained as a diagnostic fallback.

Thanks to everyone testing Try Omarchy on real hardware and over remote sessions.

## v0.0.6-preview - 2026-08-29

- Added a stable launcher under `%LOCALAPPDATA%\TryOmarchy` with optional Start-menu and Desktop shortcuts selected inside the branded setup window.
- Added an app compatibility guide covering Arch packages, VS Code, and current VM limitations.
- Added an optional instant trial account that skips the first-boot form and lands directly on the desktop.

Thanks to [Marx-Bray](https://github.com/Marx-Bray) for suggesting the launcher shortcuts in [issue #1](https://github.com/tsouth89/try-omarchy-windows/issues/1), and to everyone testing Try Omarchy across different Windows setups.

## v0.0.5-preview - 2026-08-29

- Reworked the setup splash with a clear SUPER-key explanation and starter shortcuts.
- Added safe cancellation that stops active downloads, removes partial setup data, and keeps the launcher.
- Authenticated the release manifest before downloading payloads and added recovery for incomplete installs.
- Prevented QEMU from trapping the Windows cursor when Try Omarchy is used over RDP.
- Documented essential keys, uninstalling, compatibility expectations, and common questions.

Thanks to [Tom Ballard](https://github.com/tcballard) for the release-manifest hardening and incomplete-install recovery in [PR #2](https://github.com/tsouth89/try-omarchy-windows/pull/2), and to everyone who tested the early previews and reported rough edges.

## v0.0.4-preview - 2026-08-29

- Kept the progress window visible until Omarchy opened.
- Sized guest memory to what the PC could spare and retried with less when needed.
- Kept setup errors visible above other windows.

## v0.0.3-preview - 2026-08-29

- Shipped the signed one-file Windows launcher.
- Added GPU runtime setup, graceful shutdown, clipboard sharing, folder sharing, and reliable guest reboot handling.
- Added the Omarchy 4.0.1 guest image used by later launcher releases.

## v0.0.2-preview - 2026-08-28

- Updated the guest to Omarchy 4.0.1 with all upstream themes.
- Added screensavers, autologin, clipboard sharing, host-folder mounting, and a visible SDL cursor.

## v0.0.1-preview - 2026-08-28

- First developer preview of Omarchy running under QEMU and WHPX on Windows.
