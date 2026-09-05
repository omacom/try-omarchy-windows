# Changelog

## Unreleased

### Features
- Automatic rendering now remembers when this PC cannot run the GPU path and goes straight to CPU rendering on later launches, retrying after a runtime or display-driver change, once a day, or when GPU is chosen in the new Rendering setting. A pending runtime update on a PC that already runs on CPU rendering is kept instead of being rolled back and downloaded again on every launch.
- Guest CPUs and RAM are sized to the machine: all logical processors but two (between two and eight) and a third of the RAM (4 to 8 GiB on CPU rendering; GPU rendering keeps its 6 GiB), with a Guest CPUs setting and `-cpus` to override.
- On CPU rendering the desktop starts with Hyprland animations off, since every animation frame is CPU time on llvmpipe; a choice in `looknfeel.lua` still wins. Needs the current guest image.
- `TryOmarchy.exe -reclaim` while Omarchy runs gives the space of deleted Omarchy files back to Windows: the guest writes zeros over its free space within a budget the Windows drive can spare, and after the next shutdown the launcher turns those zero blocks back into holes in the disk file. Needs the current guest image.
- A small guest agent keeps the Omarchy clock in step with Windows: the launcher sends the host time when the guest connects, every five minutes, and right after Windows resumes from sleep, and the guest corrects itself when it has drifted by more than two seconds. Existing guests gain this with the next guest-image update.
- Images now cross the clipboard in both directions: a screenshot or picture copied in Windows pastes into Omarchy as PNG, and an image copied in Omarchy pastes into Windows apps. Text keeps working as before. Existing guests gain this with the next guest-image update.
- The guest follows the Windows time zone and default keyboard layout. Each is applied when it changes on the Windows side, so a layout or zone chosen inside Omarchy stays until Windows changes; `-timezone` and `-keyboard` override this for a launch. Existing guests gain this with the next guest-image update.
- The VM window remembers its size and position: a windowed launch reopens where the window was last left when that spot is still on a connected display, with the guest console sized to match.
- Try Omarchy registers under Windows Apps & features and can be removed from there, from **Remove Try Omarchy** in Settings, or with `-uninstall`. Removal offers a full backup first and deletes only this installation's shortcuts, registry entry, saved location, and data folder.

### Fixes
- Restore now budgets free space from the backup's compressed size instead of the sparse files' nominal size, so a 10 GB backup no longer demands 33 GB free. Restored files keep the modification times their receipts recorded, so a restored copy does not re-download its image and runtime on first launch, and a restore from Settings no longer asks about shortcuts again.
- New guest users no longer start with the "no gaps" and "single-window aspect ratio" Hyprland toggles switched on, which silently overrode gaps, border size, and rounding set in `~/.config/hypr/looknfeel.lua` (#32). Existing guests keep their current toggles; run `omarchy-hyprland-window-gaps-toggle` once to turn gaps back on.
- The Settings text under the SSH key row is no longer painted over by the label above it, and messages logged before the session log opens, such as the restored-payload decision after an interrupted update, now appear at the top of the log.

## v0.0.12-preview - 2026-09-05

### Features
- Added backup, restore, and reset controls to Settings for stopped standard installs, plus `-backup` and `-restore` command-line options. Backups include the guest disk, boot files, bundled runtime, and settings, and every file is checksum-verified during restore. Restore creates a separate installation with its own launch and Settings shortcuts, so existing installations and backups are never replaced.
- Reset now offers a full backup first, prepares the new disk before moving the old one, and keeps the previous disk in a recovery folder. A failed or cancelled backup stops the reset.
- Added disk-capacity controls for standard installs, with in-place growth, current capacity and Windows free-space information. Lowering the setting never shrinks an existing disk.
- New standard installs now ask whether to use the default Local AppData folder or a different local drive or folder before downloading the runtime and guest image. Alternate locations are checked for write access and free space, remembered across launches, and carried into Start-menu and Desktop shortcuts.
- Install locations and shared folders are now chosen with the Windows folder picker. Unreadable preferences prompt an optional repair that preserves the original file and leaves guest files untouched.
- The app icon, setup splash, and VM window now use the official Omarchy mark.

### Fixes
- Added stable-release update support, including a bridge for older preview launchers and recovery-state compatibility. Stable installs stay on stable releases.
- Fixed clipboard sharing after reconnects and when copying an earlier value again. Guest copies keep trailing newlines, failed sends are retried, and overlapping transfers no longer suppress a later copy. Existing guest disks receive the updated bridge with this release's guest payload.
- Interrupted setup can reuse a completed download after a server outage or an ignored resume request, with the checksum verified before use. Failed runtime extraction keeps the verified archive for the next attempt, and short disk writes or oversized responses are detected.
- Cancelling setup on an existing installation now removes only launcher staging files. Unrelated `.part` files, shared folders, retained recovery data, and linked guest folders are left alone.
- Portable reset now prepares the new disk before retaining the old disk and its backing identity in a recovery folder, and rolls back if publication fails. Recovery data also survives a cancelled setup after an interrupted reset.
- Updated active download, update, issue, clone, and module links after the repository moved to `omacom`. Signed update manifests and existing guest and runtime receipts remain compatible with the old release base, so the transfer does not force a payload refresh or strand older launchers.

Thanks to [tcballard](https://github.com/tcballard) for the official Omarchy mark in the app branding, [7Wdev](https://github.com/7Wdev) for requesting install-location and disk controls, and [Sperum](https://github.com/Sperum) for the backup request behind the new backup and restore controls.

## v0.0.11-preview - 2026-09-03

### Features
- Standard installs now offer a dedicated `Omarchy Shared` folder for moving files between Windows and Omarchy. It is opt-in, can be disabled without forgetting the path, opens in Files the first time it is attached, and stays pinned in the sidebar without replacing user bookmarks.
- Added a tray menu while Omarchy is running for reopening the VM, opening the shared folder, Settings, diagnostics, and clean shutdown. Start-menu installs also receive a separate Settings shortcut, including existing installs on their next successful launch.

### Fixes
- Fixed upgraded guest disks skipping newer launcher integration when the Linux kernel version had not changed. Existing v0.8 through v0.10 guests now receive the shared-folder link and Files bookmark without replacing the guest or user data.
- Kept folder sharing available with CPU rendering when the bundled WINQ-EMU runtime is installed.
- Invalid or unavailable saved folders no longer prevent Omarchy from starting. Unsafe broad, system, network, reparse-point, and VM-data paths are rejected before launch.
- Fixed Settings opening behind the maximized Omarchy window when selected from the tray.
- Refreshed the locked guest packages for Mesa 26.2.2, WirePlumber 0.5.17, and GNOME Autoar 0.5.2.

Thanks to [majilesh](https://github.com/majilesh) for portable USB mode, [Tom Ballard](https://github.com/tcballard) for disk-space preflight, [Pedro Perez](https://github.com/pjperez) for ARM64 host detection, and [Chainfire](https://github.com/Chainfire) for WHPX guidance. Thanks also to [Jocelyn Legault](https://github.com/joce), [eskwayrd](https://github.com/eskwayrd), [Anees Khan](https://github.com/aneeskhan47), and [Brady Walsh](https://github.com/knighthawkbro) for reports that led to fixes in this release.

## v0.0.10-preview - 2026-09-03

- Withdrawn during prerelease testing because upgraded guest disks could miss the new Files integration. Superseded by v0.0.11-preview.

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
- Shipped Omarchy's LazyVim configuration and clang in new and reset guest images, so Neovim starts with the expected setup and Tree-sitter can compile parsers.
- Fixed screen recording in new and reset guest images, which never started because the recorder was missing, and shipped the other tools Omarchy's keybindings and menus expect: the screenshot editor, OCR and QR capture, emoji and clipboard paste, man pages, the calculator, writer and video trimmer, Herdr, and the screen-share picker.
- Kept launcher and guest-image rollback active until the booted guest reaches userspace and networking. QEMU's control socket alone can answer during a kernel panic, so it is no longer treated as proof that an update is healthy.
- Preserved existing writable guests when a release raises the virtual disk size. The launcher now grows the disk in place instead of mistaking it for an incomplete first-run copy and replacing it with the factory image.
- Bound portable QCOW2 data to the authenticated factory-image digest so replacing its backing payload is refused instead of risking silent filesystem corruption.
- Rebuilt the Windows runtime to avoid 1 ms SDL redraw polling while the guest is idle. Real-hardware idle CPU and graphics checks still gate publication.
- Raised the guest PipeWire quantum to prevent false underruns from QEMU's coarse emulated HDA position updates.
- Backported Omarchy's notification close control and kept notification contents hidden while the lock screen or screensaver is active.
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

Thanks to [Tom Ballard](https://github.com/tcballard) for resumable downloads in [PR #7](https://github.com/omacom/try-omarchy-windows/pull/7), and to everyone who reported Windows shortcuts leaking through while Omarchy was running.

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

Thanks to [Marx-Bray](https://github.com/Marx-Bray) for suggesting the launcher shortcuts in [issue #1](https://github.com/omacom/try-omarchy-windows/issues/1), and to everyone testing Try Omarchy across different Windows setups.

## v0.0.5-preview - 2026-08-29

- Reworked the setup splash with a clear SUPER-key explanation and starter shortcuts.
- Added safe cancellation that stops active downloads, removes partial setup data, and keeps the launcher.
- Authenticated the release manifest before downloading payloads and added recovery for incomplete installs.
- Prevented QEMU from trapping the Windows cursor when Try Omarchy is used over RDP.
- Documented essential keys, uninstalling, compatibility expectations, and common questions.

Thanks to [Tom Ballard](https://github.com/tcballard) for the release-manifest hardening and incomplete-install recovery in [PR #2](https://github.com/omacom/try-omarchy-windows/pull/2), and to everyone who tested the early previews and reported rough edges.

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
