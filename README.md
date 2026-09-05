<p align="center">
  <img src="app/OmarchyIcon.svg" width="128" height="128" alt="Try Omarchy logo">
</p>

<h1 align="center">Try Omarchy for Windows</h1>

Run the full [Omarchy](https://omarchy.org) desktop in a window on Windows 10 or 11. No VMware, no VirtualBox, no dual boot: QEMU on the Windows Hypervisor Platform (WHPX), a prebuilt Arch image with Omarchy baked in, and the desktop rendered on your actual GPU (virgl + Venus Vulkan via [WINQ-EMU](https://github.com/cmspam/winq-emu)) with CPU rendering as the automatic fallback. No partitions, no bootloader, no changes to your Windows install: everything lives in one folder chosen on first run, with `%LOCALAPPDATA%\TryOmarchy` as the default.

Download, boot, Hyprland.

![The Omarchy desktop running in the Try Omarchy window on Windows](docs/images/hero.jpg)

![Live capture on the Ryzen 5 test laptop: fastfetch, the Omarchy menu, and a screensaver inside the Try Omarchy window](docs/images/demo.gif)

**Status: working end to end on real hardware.** One app switches on Windows' virtualization, downloads the GPU runtime and the image, boots, and supervises; the desktop renders on the GPU and falls back to CPU rendering automatically. Landing page: [tryomarchy.com](https://tryomarchy.com). See the [changelog](CHANGELOG.md) for release history.

Try Omarchy for Windows is maintained under [Omacom](https://github.com/omacom),
alongside [Try Omarchy for macOS](https://github.com/omacom/try-omarchy).
The Omarchy mark in the app icon is sourced from the
[official Omarchy brand kit](https://omarchy.org/brand/) and remains subject to
Omarchy's trademark rights.

## What works today

- **The full Omarchy 4.0.2 desktop on new or reset guests**: Hyprland, the bar, notifications, all 22 themes, the screensavers. On our mid-range Ryzen 5 test laptop the desktop is up about 6 seconds after launch, and every launch after setup goes straight there. No Linux login screens, no console text, branded window.
- **GPU acceleration**: Hyprland renders on the host GPU via virgl, `vulkaninfo` shows Venus, smooth video and audio (verified on a Radeon iGPU laptop); `-cpu host` (AVX2 and all) via WINQ-EMU's patched WHPX.
- **One app, zero prerequisites**: `TryOmarchy.exe` (~8 MB, no console window). First run lets you keep the default Local AppData location or choose another local drive or folder, switches on Windows' Hypervisor Platform (one permission prompt, one restart), then downloads the SHA256-verified GPU runtime and image and boots into Omarchy's setup form. Once setup is complete it keeps a stable launcher in the chosen data folder and can add optional Start-menu and Desktop shortcuts. After that it supervises everything: GPU/CPU auto-detect, the known WHPX launch wedge, in-guest reboot relaunch, poweroff cleanup.
- **Feels like an app, not a VM**: the window is branded "Try Omarchy", the Windows key acts as Super only while the window is focused (Start menu and Win+Shift+S keep working everywhere else), Ctrl+Alt+F goes fullscreen.
- **Two-way text clipboard sharing** between Windows and Omarchy (own compositor-native bridge over wl-clipboard, no SPICE) and **folder sharing** over virtio-9p: standard installs offer to create `Omarchy Shared` in your Windows home, then pin it in Omarchy's Files sidebar and link it into the Linux home. The tray can open the Windows folder at any time. File clipboard and drag-and-drop are not supported yet; use the shared folder to move files.
- First boot offers an instant trial account or Omarchy's normal personalized account setup, with SDDM autologin after either path. Instant mode keeps `omarchy` as both the local username and lock-screen password, shows that on the setup splash, and repeats it once on the first desktop. Sudo remains passwordless in this disposable local trial.
- Reproducible x86_64 guest image build (containerized, package-locked, pinned Omarchy revision) and a headless QMP control plane for automated testing.

See [app compatibility](docs/COMPATIBILITY.md) for package support and current VM limitations. The [v1 checklist](docs/V1-READINESS.md) tracks the remaining release work.

| First run | Screensaver |
|---|---|
| ![Omarchy first-run setup inside the Try Omarchy window](docs/images/first-run.jpg) | ![Omarchy pixel-logo screensaver](docs/images/screensaver.jpg) |

## Essential keys

- **Windows key** acts as Super, but only while the Try Omarchy window is focused. Everywhere else it stays your normal Windows key, so the Start menu and Win+Shift+S keep working.
- **Ctrl+Alt+F** fullscreens the VM window itself on your Windows desktop (SUPER+F, below, is the in-Omarchy one).
- **Ctrl+Alt+G** grabs or releases raw keyboard input. If the host steals a shortcut you meant for Omarchy, grab first. Same trick if you're driving the VM over VNC or RDP and focus gets weird.
- Hyprland is keyboard-first by design and the first hour is the adjustment period. Learn two keys and the rest follows: **SUPER+SPACE** opens the Omarchy menu, **SUPER+K** opens the keybinding viewer with every binding and its description. The everyday starters: SUPER+RETURN opens a terminal, SUPER+W closes the focused window, SUPER+F fullscreens it.

## Architecture

Same recipe as the excellent macOS [try-omarchy](https://github.com/themartiano/try-omarchy) (QEMU + Apple Hypervisor Framework + VirGL), translated to Windows:

| Piece | macOS (try-omarchy) | This project |
|---|---|---|
| Hypervisor | Hypervisor.framework | Windows Hypervisor Platform (WHPX) |
| Guest image | ARM64 Arch + Omarchy | x86_64 Arch + Omarchy |
| Graphics | VirGL | virtio-gpu virgl + Venus Vulkan (WINQ-EMU); llvmpipe fallback |
| App shell | Swift/AppKit | Go: one console-less `TryOmarchy.exe` (PowerShell scripts remain as a fallback path) |

WHPX works on Windows Home and Pro (it's the same platform WSL2 rides on), so no Hyper-V role is required. If WSL2 runs on your machine, you're set.

Proven boot recipe: `-accel whpx -machine q35 -cpu qemu64`, direct kernel boot (vmlinuz + initramfs + raw ext4 rootfs on virtio-blk), all-virtio devices, DirectSound audio. See [docs/FINDINGS.md](docs/FINDINGS.md) for the details and the traps.

## Try it

Download [TryOmarchy.exe](https://github.com/omacom/try-omarchy-windows/releases/latest/download/TryOmarchy.exe) (~8 MB, [SHA256](https://github.com/omacom/try-omarchy-windows/releases/latest/download/TryOmarchy.exe.sha256)) and open it. First run asks where to store the virtual machine, graphics runtime, and downloads. Use the default Local AppData location or choose another local drive or folder. Windows then asks permission to switch on the Hypervisor Platform and restarts once, after which the app pulls the GPU runtime (a portable [WINQ-EMU](https://github.com/cmspam/winq-emu) tree, ~84 MB) and the Omarchy image (~1.7 GB), everything SHA256-verified. Choose the instant trial account to go straight to the desktop, or use Omarchy's setup form to choose your own account. Every launch after goes straight to the desktop.

After the first successful setup, Try Omarchy offers optional Start-menu and Desktop shortcuts. Start-menu installs include a separate settings shortcut. They point to a stable copy of the signed launcher in the chosen data folder, so the original download can be moved or deleted. Opening a newer downloaded release refreshes that stable copy.

While Omarchy is running, the Try Omarchy tray icon can reopen its window, open the active shared folder, open Settings, create a diagnostics bundle, or request a clean shutdown.

Try Omarchy checks for updates when it starts. Release metadata is signed with a separate Ed25519 update key, and its authenticated hashes cover the signed launcher and the guest payload manifest. New files are fully downloaded and verified before they replace anything. The previous launcher, bundled runtime, and factory image remain available until the updated VM reaches a healthy boot, while `vm\disk.raw` is left untouched. If the first boot fails or is interrupted, the next launch restores the previous files automatically. Use `-no-update` when an offline or version-pinned launch is required.

Already have WINQ-EMU at `C:\WINQ-EMU`, or stock QEMU from the old bootstrap? The app prefers what's installed and downloads nothing extra.

Prefer to build the app yourself? Any machine with Go, then run the exe on Windows:

```
git clone https://github.com/omacom/try-omarchy-windows
cd try-omarchy-windows/app
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o TryOmarchy.exe .
```

The launcher embeds and pins the SHA256 digest of the default release's
`SHA256SUMS` file. It verifies an existing cache before trusting it, records a
manifest-bound install receipt for fast offline launches, and only promotes
fully written rootfs and writable-disk staging files into place. When publishing
a new image release, update `defaultReleaseURL`, `defaultSumsSHA256`, and the
matching fixture in `app/testdata`, plus `currentVersion` in `app/update.go`.
Interrupted payload transfers resume from their `.part` files when the server
supports byte ranges, then the complete file is SHA256-verified before use.
Custom release URLs must be paired with the
trusted manifest digest via `-sums-sha256`.

### Reporting a problem

Run `TryOmarchy.exe -diagnostics`. It writes one zip under the chosen data
folder's `diagnostics` directory with the launcher and QEMU logs, the
guest's console output, redacted settings, install and update state, the guest
manifest, and machine facts (Windows build, CPU, memory). It includes no disk
images or home-folder files and redacts known account paths and SSH key data.
Logs can still contain local details, so review the zip before attaching it.

### Install location

New standard installs ask for a data location before downloading anything. The
default is `%LOCALAPPDATA%\TryOmarchy`. Choosing another local drive or folder
creates a `TryOmarchy` folder there and keeps a small
`%LOCALAPPDATA%\TryOmarchy\data-location.json` pointer so direct launches can
find it. Standard installs require an NTFS or ReFS local drive because the
virtual disk uses sparse files. Network locations are not supported. Existing
installs stay where they are, and an explicit `-dir PATH` still wins for that
launch. Portable mode continues to support exFAT through the `data` and
`payload` folders beside the executable.

### Settings

`settings.json` in the chosen data folder keeps the choices that survive a
relaunch. Every row has a matching flag, and a flag given on the command line
wins for that launch:

```json
{
  "schemaVersion": 1,
  "fullscreen": false,
  "memoryMiB": 0,
  "cpus": 0,
  "share": "",
  "shareDisabled": false,
  "sharedFolderPrompted": true,
  "forwards": ["tcp:2222:22"],
  "sshKey": "",
  "render": "auto"
}
```

`fullscreen` is the Immersive mode (`-fullscreen`), `memoryMiB` overrides the
automatic guest RAM sizing (`-memory`, 0 keeps it automatic), `cpus` overrides
the automatic vCPU count (`-cpus`, 0 keeps it automatic), `share` remembers
the Windows folder shared into Omarchy (`-share`), `shareDisabled` turns that
folder off without forgetting it, and `forwards` are loopback port
forwards (`-forward`), and `sshKey` is the public key file to authorize when a
forward targets sshd (`-ssh-key`), and `render` picks the rendering path
(`-render`). Open Settings from the tray, the Start menu, or
`TryOmarchy.exe -settings`. Changes apply on the next launch.

`render` is `auto` by default: the launcher tries GPU rendering and, when this
PC cannot run it, remembers that in `render-probe.json` so later launches go
straight to CPU rendering instead of repeating the failed attempts. It retries
the GPU path when the runtime or the display drivers change, and once a week.
`gpu` retries every launch; `cpu` never tries it (`-nogpu` means the same).

Automatic sizing gives the guest all logical processors but two, between two
and eight, and a third of the machine's RAM between 4 and 8 GiB (up to 12 GiB
with GPU rendering), reduced to what Windows can spare at launch.

### Disk capacity

Open Settings and set **Disk capacity (GiB)**, or launch with `-disk-size 64`.
Standard installs accept 24 to 1024 GiB; 0 keeps the release default. The next
launch grows an existing disk in place and preserves its files. Lowering the
setting never shrinks the disk. A fresh guest uses at least the factory image's
required capacity.

Capacity is a limit, not space reserved on Windows. The sparse disk uses host
storage as you add files. Settings shows the current capacity and free space on
the Windows drive. Keep important files backed up outside the guest.

This preference is saved separately in `storage.json` so older launchers can
still read their settings after rollback. An explicit `-disk-size` applies only
to that launch. Portable QCOW2 disks keep their existing capacity.

### SSH and port forwarding

Nothing listens by default. To reach Omarchy from Windows tools, forward a
loopback port:

```
TryOmarchy.exe -ssh 2222
```

That forwards `127.0.0.1:2222` to Omarchy's sshd for this session only and
asks the guest to start sshd for that boot. Nothing on your network can reach
it. Your `~/.ssh/id_ed25519.pub` (or `id_ecdsa.pub`, `id_rsa.pub`) is authorized
for the Omarchy account automatically; pass `-ssh-key PATH` to pick another
public key, or use none and log in with the password you chose in Omarchy.
Then:

```
ssh -p 2222 <omarchy-user>@127.0.0.1
```

The same alias works for `scp`, Git, and VS Code Remote SSH. Other services
use `-forward tcp:8080:80` or `-forward udp:5000:5000` (repeatable); the guest
service must listen on its network interface, not only on its own localhost.
From Omarchy, `10.0.2.2:<port>` reaches a service on Windows without any
mapping. Key-only or permanent SSH is Omarchy's own choice: run
`omarchy-setup-security-sshd` inside the guest. A fresh disk (`-fresh`) gets a
new host key, so remove the old `[127.0.0.1]:2222` entry from `known_hosts`
if ssh complains.

### Taking your setup to a real Omarchy install

Inside Omarchy, run `try-omarchy-export`. It writes one archive with your
desktop configuration, theme, and the packages you added, to the shared Windows folder
when one is mounted (`-share`) or to your home folder otherwise. On the real
install, extract it and run the `restore.sh` inside. Keys, password stores,
browser profiles, and unlisted application configs are deliberately left out.
Review the archive before sharing it with anyone. See
[`docs/MIGRATION.md`](docs/MIGRATION.md).

### Offline portable mode

The launcher also accepts `-portable` for an experimental, persistent USB
layout. In this mode it reads an authenticated release payload beside the
executable, makes no setup-time network requests, stores all guest state on the
removable drive, and uses a compact QCOW2 overlay that survives Windows drive
letter changes and works on exFAT. The independently pinned `SHA256SUMS` digest,
install receipts, cancellation handling, and atomic file publication apply to
the portable path too.

See [`docs/PORTABLE_USB.md`](docs/PORTABLE_USB.md) for the expected layout and
host requirements. Bundle preparation and additional host launchers are kept
out of this core Windows change so they can be reviewed separately.

Or skip the app and drive QEMU from PowerShell: `scripts\bootstrap.ps1` then `scripts\launch-omarchy.ps1` (elevated).

## VM backups

Backup, restore, and reset controls are available in Settings for stopped
standard installs. Restore creates a separate copy. Command-line options
are also available. See the [backup guide](docs/BACKUP.md) for usage,
storage requirements, and current limitations.

## FAQ

### Isn't this just QEMU in disguise?

Yes, and that's the point. QEMU on WHPX is the best virtualization stack Windows has, but wiring it up yourself (machine type, virtio devices, GPU forwarding, input handling, the known launch wedges) is a weekend project on its own. The app does that wiring for you, supervises the VM, and keeps everything in one folder you can delete.

### Why is the download only ~8 MB?

TryOmarchy.exe is just the launcher. On first run it fetches the GPU runtime (~84 MB) and the Omarchy image (~1.7 GB), SHA256-verifies both, and caches them in the data folder you chose. After that, launches work offline.

### Why not just use a live USB?

A live USB means rebooting away from your machine and forgetting everything on shutdown. This runs in a window next to your actual work, keeps your state between sessions, and renders on your real GPU.

### What are the instant trial credentials?

The local trial account is named `omarchy` and its lock-screen password is `omarchy`. Sudo does not ask for a password in instant trial mode. Try Omarchy does not enable SSH or expose inbound network ports unless you ask for a forward with `-ssh` or `-forward`, and those bind to `127.0.0.1` only.

### How do I remove Try Omarchy?

Delete the data folder you chose during setup. That removes the launcher, runtime, image, and writable virtual disk. If you used an alternate location, also delete `%LOCALAPPDATA%\TryOmarchy\data-location.json`. If you created Windows shortcuts, remove them from the Start menu or Desktop like any other shortcut. The original downloaded `TryOmarchy.exe` can be deleted separately.

### I have the full Hyper-V feature set installed. Will it conflict?

WHPX and Hyper-V share the same Windows hypervisor and are designed to coexist. We have not yet validated every Try Omarchy feature on a machine with the full Hyper-V feature set enabled, so please open an issue if you hit anything odd.

## Repository layout

- `app/` — the app itself: one Go exe covering the launcher, supervisor, first-run download, focus-scoped Win-key forwarding, and the host side of the clipboard bridge
- `runtime-build/`: the source-locked Windows QEMU runtime build, verification, licenses, and provenance tooling
- `scripts/` — PowerShell path plus QMP tooling (screendump, send-key, WHPX smoke test)
- `guest-build/` — patches on jorge's guest builder that produce our image, plus build instructions
- `docs/FINDINGS.md` — technical findings, gotchas, and their fixes
- `docs/RELEASING.md` - the authenticated two-phase build, signing, and publishing process

The guest image (Omarchy 4.0.2, all upstream themes, screensavers, autologin, clipboard bridge) is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s `win` branch guest builder (`guest/build-container.sh`, needs Docker on Linux) — an x86_64 retarget of the upstream try-omarchy build system — with the patches in `guest-build/` applied. Images are not committed; setup downloads the latest release artifact, or build your own.

## Credit where due

This project stands on a lot of shoulders:

- [Omarchy](https://github.com/basecamp/omarchy) by DHH / Basecamp — the desktop this is all about
- [try-omarchy](https://github.com/themartiano/try-omarchy) by Eduardo (themartiano) — the original macOS app and the architecture this follows
- [try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win) by Jorge Silva — the x86_64 guest builder retarget and the proven WHPX boot recipe this project reuses
- [WINQ-EMU](https://github.com/cmspam/winq-emu) by cmspam — Venus Vulkan GPU forwarding for QEMU on Windows, the graphics path
- [omarchy-windows-hyperv-gpu](https://github.com/Chainfire/omarchy-windows-hyperv-gpu) by Chainfire — prior art proving GPU-accelerated Omarchy on Windows, plus the QEMU 11 WHPX interrupt findings
- [dockur/windows](https://github.com/dockur/windows) — the Windows-in-Docker environment this is developed and tested in

Open to collaboration — if you're working on any of this, get in touch.

## License

Scripts and docs in this repo: [MIT](LICENSE). Omarchy and the guest image contents carry their own licenses.
