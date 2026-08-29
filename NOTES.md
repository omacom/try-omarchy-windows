# try-omarchy-windows — working notes / session handoff

**Read this first in a new session.** Keep this file updated as work progresses.

## Session 2026-08-28/29 (laptop): one-click validation pass (HANDOFF steps)

- Step 1 ✓ release exe hash-verified from tryomarchy.com/download.
- Step 2 regression ✓ (C:\WINQ-EMU wins, no downloads, GPU; provisioning driven
  blind over QMP). Step 3 runtime download ✓ after fixing a real bug: Defender
  holds handles on freshly unpacked binaries and the runtime.part->runtime
  rename got access-denied (retry loop shipped; fell back to CPU mode cleanly
  when it failed — fallback path also proven).
- Landed mid-pass on user feedback: close guard (X/Alt+F4 -> owned confirm ->
  graceful QMP shutdown; window-close=off), taskbar identity (Win11 taskbar
  ignores WM_SETICON; window property store AUMID + RelaunchIconResource),
  multi-size icon.ico, panic traces to shell.log (the shell once died silently
  mid-session leaving QEMU orphaned - windowsgui panics went nowhere).
- Step 4 bare-machine: laptop CANNOT go bare — see the new FINDINGS entry
  "WHPX needs the hypervisor, not the feature": VBS/HVCI keeps the hypervisor
  (and thus WHPX) alive with the feature disabled AND hypervisorlaunchtype
  off. Product story upgrade: VBS-on machines (Win11 default) need ZERO setup.
  The enable flow stays VM-validated (dockur); hardware skip accepted by user.
- The release binary for signing = local build of master (166d2642...), which
  superseded the uploaded dd1c64 asset (close guard, icons, panic logging,
  rename retry all landed during the pass). Step 5 signing in progress.
Last updated: 2026-08-28 late night (laptop: everything hardware-validated —
v0.0.2 image, v0.0.3 image, TryOmarchy.exe incl. real double-click first run;
shell splash restyled + icon). **State: preview-release quality end to end.
Remaining before announcing: Linux box rebuilds the exe from current master
(splash + fixes) and decides packaging/signing; see "Open work".**

## Session 2026-08-28 late night (laptop): double-click reality check + splash

The real-user double-click test of TryOmarchy.exe found two shipping bugs the
VM validation missed, then the setup UI got the Omarchy look. All fixed,
verified on hardware, pushed (through 80d533e):

- **The first-run progress window never existed.** ui.go's WNDCLASSEXW mirror
  declared cbClsExtra/cbWndExtra pointer-sized; they are 4-byte C ints, so
  cbSize came out 88 (not 80) and RegisterClassExW rejected the class -
  silently, since no return was checked. A real double-click showed NOTHING
  while the 1.4 GB download ran blind. Lesson: every Win32 struct mirror is
  guilty until its Sizeof matches the C headers, and every .Call() return gets
  checked and logged.
- **Double-click twice = two blind downloads into the same files.** The
  single-instance guard (lifecycle port 4450) bound only after ensureGuest;
  it now binds first, and a second instance dies in <1s with the
  already-running dialog (verified).
- **Setup splash restyled** (user: "clean and modern like omarchy" - the grey
  stock dialog "looks bad and in development"): borderless dark panel, Tokyo
  Night palette, pixel-art O + green OMARCHY wordmark + tagline, live status,
  slim SELF-DRAWN progress bar (the classic themed bar paints a light border
  on dark - draw it in WM_PAINT instead), Win11 rounded corners
  (DWMWA_WINDOW_CORNER_PREFERENCE), drag-anywhere (WM_NCHITTEST->HTCAPTION),
  Esc cancels, SS_NOPREFIX so "&" renders. RegisterClassExW tolerates
  ERROR_CLASS_ALREADY_EXISTS (the class registers twice per run: download UI,
  then disk-prep UI).
- **App icon**: app/icon.ico (chunky pixel O on dark rounded square, generated
  by a throwaway System.Drawing script + ffmpeg png->ico) embedded via
  app/rsrc_windows_amd64.syso (github.com/akavel/rsrc) - Explorer/taskbar
  icon + splash window icon. Committed so the Linux cross-build picks it up.
- Laptop build recipe: Go 1.27.0 official zip (no admin) ->
  %LOCALAPPDATA%\go-toolchain; `go build -trimpath -ldflags '-s -w
  -H windowsgui' -o TryOmarchy.exe .` in app/.
- Hardware validation recap for the exe (earlier tonight, plus this round):
  66s first-run image fetch, attempt-1 boots every time (10s QMP grace),
  lifecycle reboot-notify verified, all 9 launch-UX checks pass, **audio good,
  winkey good (user-confirmed)**, real double-click first run now shows the
  splash front and center (user-confirmed working).
- UI iteration trick: a scratch harness copying ui.go+winapi.go with a fake
  driver main() lets the splash render with simulated progress without
  touching a running instance (single-instance port) or downloading anything.

## Session 2026-08-28 night (Linux box): the native app shell (app/)

`app/` is now a real Go app shell — ONE console-less TryOmarchy.exe (~7 MB,
zero cgo, cross-compiled from Linux with `GOOS=windows go build`) that replaces
launch-omarchy.ps1 + winkey-forwarder.ps1 + clipboard-bridge.ps1 AND
bootstrap's image download. What it does, all VM-validated tonight:

- First run: downloads the guest image from the GitHub release (progress
  window, SHA256-verified, zstd-unpacked sparse, .zst deleted after), then
  boots. Verified against the live v0.0.2-preview release end to end.
- Launch: GPU auto-detect (WINQ-EMU) / stock CPU fallback, sparse disk prep,
  silent-boot cmdline, supervisor with launch watchdog, reboot relaunch,
  poweroff-wedge reap, single-instance port check, qemu stderr captured to
  vm\qemu-stderr.log, shell log in vm\shell.log.
- In-process winkey forwarder (focus-scoped LL hook, front-of-chain rehook),
  "Try Omarchy" title enforcement, host clipboard bridge (verified: the baked
  guest service connected on its own), audio fallback dsound -> none.
- MAJOR find: the WHPX "launch wedge" is CAUSED by early QMP connections —
  probe at t=1.5s wedged 8/8 nested launches; wait 10s and it's healthy on
  attempt 1 every time. Plus: virtio-sound without -audiodev hangs the guest
  session (both writeups in FINDINGS.md).
- Reboot-vs-poweroff on a wedged stock QEMU is solved via the image:
  guest-build patch 0004 adds try-omarchy-reboot-notify (lifecycle port 4450).
  SHIPPED as **v0.0.3-preview** (same 4.0.1 image + patches 0003/0004),
  KVM-validated end to end including the reboot notification landing on the
  host listener. bootstrap.ps1 and the exe's default -release now point at it.
- Testing trick worth keeping: the dockur VM console can be driven without ssh
  via the container's QEMU monitor socket (docker exec + nc to
  /run/shm/monitor.sock: sendkey/screendump), and apps land on the interactive
  desktop via `schtasks /run`. Used tonight to fix the VM's sshd (a Windows
  update silently removed the OpenSSH.Server capability — remove + re-add the
  capability restores it) and to validate the SDL window + title.
- Still laptop-only: GPU mode, real audio (the dsound path), SDL window
  foreground behavior from a normal double-click, -share end to end (the
  automount is in the v0.0.2 image), winkey feel, -fullscreen.

## Session 2026-08-28 evening (Linux box): image rebuild, v0.0.2-preview shipped

Everything on the Track 2 list landed in one rebuild, validated end to end, and
published: https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.2-preview

- Builder changes live as commits on branch `image-v0.0.2` of the local
  try-omarchy-win checkout AND as `guest-build/*.patch` in this repo (apply with
  `git am` onto jorge's win branch — see guest-build/README.md).
- Omarchy pinned to the v4.0.1 release tag. Upstream's `version` FILE still says
  4.0.0.alpha at that tag — the builder now records that as `versionFile` and
  stamps the release version into the staged runtime, so `omarchy-version`
  reports 4.0.1-1 in-guest (verified).
- All 22 upstream themes ship (4.0.1 grew the set from 6). Cost: rootfs.ext4.zst
  1.28G -> 1.34G. Total download ~1.41G — inside the "under 2GB, no 3GB bloat"
  budget. Distribution model stays stub-style: bootstrap.ps1 is the tiny
  download, the image comes down during setup.
- Autologin: upstream 4.0.1 provisioning already writes /etc/sddm.conf.d/
  autologin.conf itself, but arms omarchy-provision-autologin-once.service to
  delete it on the second boot (unencrypted installs). Our image adds an
  ExecStartPost drop-in (keep-autologin) that disarms that cleanup — autologin
  is permanent, greeter never appears. Verified across a reboot.
- Clipboard bridge baked in: /usr/local/bin/clipboard-bridge + user unit enabled
  globally (WantedBy=graphical-session.target). Service active on first login.
  Cosmetic finding: with NO host bridge listening, the push-side socat printed a
  broken-pipe line into a terminal; silenced with 2>/dev/null (patch 0003, in
  the overlay + this repo's copy — NOT in the published v0.0.2 image; harmless
  because launch-omarchy.ps1 always runs the host side).
- 9p automount: mnt-host.mount with ConditionPathExistsGlob on virtio mount_tag
  — "inactive" (not failed) when booted without -Share. Verified.
- Cursor visible under SDL fixed in the builder fragment (was the VNC-era
  invisible=true).
- Boot-text polish WITHOUT plymouth (plymouth is NOT in the image — earlier note
  was wrong): launch-omarchy.ps1 now strips console=tty0/hvc0 from the cmdline
  (serial log keeps everything via console=ttyS0) and adds
  vt.global_cursor_default=0. Boot = black window -> branded splash/SDDM ->
  desktop. Verified under KVM with the same cmdline.
- Validation was done under KVM on the Linux host (new trick — no Windows VM
  needed for image work): host qemu + KVM boots the image with the same virtio
  device set; needed qemu-hw-display-virtio-gpu{,-pci} on Arch. Full first-boot
  form driven over QMP; scratchpad hmp.py used HMP sendkey (QMP send-key
  qcodes were flaky for Return in gum — HMP sendkey worked every time).
- Numbers (KVM, Ryzen desktop): 996ms kernel + 2.45s user = 3.45s to
  graphical.target on the provisioned second boot.
- Fresh-image validation on the laptop still pending — that's HANDOFF.md.

## Session 2026-08-28 (laptop, Tyler account)

The laptop has two Windows accounts; earlier work ran as Brandon (admin), this
session as **Tyler (non-admin)**. Notes:

- Per-user pieces had to be redone for Tyler: guest image re-downloaded from the
  v0.0.1-preview release into his `%LOCALAPPDATA%\TryOmarchy` (machine-wide QEMU /
  WINQ-EMU / WHP were already in place). zstd via winget again showed the
  missing-Links-shim issue — bootstrap's Packages-dir fallback path is correct.
- **WHPX wedge repro, fresh machine boot, non-admin:** first TWO stock-QEMU launches
  wedged at ~1.3s into kernel boot (QMP accepts the TCP connect but main loop never
  answers; window Not Responding; one vCPU spinning). Third launch booted clean.
  Same signature as the FINDINGS "leaked partition state" note but with no prior
  force-kill this boot — so the first-launch wedge is not (only) leaked state.
  Kill-and-relaunch remains the workaround; app shell needs a launch watchdog
  (QMP handshake within N seconds or kill+retry).
- Provisioned over QMP as user tyler / hostname omarchy / America/Chicago.
  Cursor-visible fix applied in-guest (monitors.lua) — still needs the image-builder
  fix. QMP typing note: don't race sudo's password prompt; also a successful sudo
  caches the token, so a queued retype of the password lands in the shell.
- **SDDM autologin applied in this VM** (`/etc/sddm.conf.d/autologin.conf`,
  User=tyler Session=hyprland-uwsm) — same recipe as the 08-27 proof; still needs
  provisioning to write it automatically. First attempt was LOST by the poweroff
  wedge (see new FINDINGS corollary: the wedge can drop recent writes — `sync`
  after anything that matters); rewritten + synced, then verified after reboot.
- Guest-initiated poweroff wedge confirmed again on stock QEMU 11.1.0 (clean guest
  shutdown, then QEMU hangs at ~0% CPU; force-killed). Guest reboot on the
  WINQ-EMU build exits QEMU cleanly (`-no-reboot`) — no wedge.
- GPU relaunch as Tyler: WINQ-EMU boots the same disk, winkey-forwarder auto-started
  and connected on QMP 4446. **Forwarder VERIFIED hands-on 2026-08-28** ("keys are
  working great"): Super reaches Omarchy when the VM is focused, Start menu and
  Win+Shift+S work normally otherwise. **Autologin VERIFIED** the same session.
- **Product feedback (2026-08-28, hands-on): "I see the QEMU screen — users should
  never see that."** Clarified: Omarchy-branded screens (its own login/lock) are
  fine; what must never appear is the generic SDDM greeter, QEMU chrome, or console
  windows. Requirement: launch → branded window → desktop. DONE so far:
  window retitled "Try Omarchy" (forwarder reasserts every 3s — QEMU rewrites its
  title on every grab toggle), consoles eliminated (launcher uses
  qemu-system-x86_64w.exe + hidden forwarder; QEMU messages go to vm\qemu.log).
  Remaining: window icon, boot-time splash (image ships plymouth — wire it up, or
  the shell covers the window until the desktop is up), console-flash before SDDM.
- **Screensavers required for release** (feedback 2026-08-28: "screensavers are a
  big part" of the full Omarchy experience). Installed live in this VM over QMP
  and synced: ttfx 0.3.2, hypridle 0.1.8, plus vulkan-virtio 26.2.1 + vulkan-tools
  for the full Venus path. All four are already on the image-builder must-add list.
- **Launcher/bootstrap polish (2026-08-28), all parse-checked:**
  - `launch-omarchy.ps1` unified + supervised: auto-detects WINQ-EMU (`-NoGpu` to
    override), windowless qemu-system-x86_64w.exe, launch watchdog (QMP handshake
    on new supervisor port 4447 within 30s or kill+retry, 4 attempts — covers the
    twice-in-a-row launch wedge seen today), reaps the guest-poweroff wedge after
    the SHUTDOWN event, relaunches on guest-reset, forwarder always on (both
    modes), virtio-sound + -smp 6 in CPU mode too. `launch-omarchy-gpu.ps1` is now
    a compat shim. Ports: 4445 tools / 4446 forwarder / 4447 supervisor.
  - `bootstrap.ps1` runs without admin when WHP+QEMU are already machine-wide
    (per-user image download + zstd fallback), and points at WINQ-EMU if absent.
  - PS 5.1 trap: em-dashes in .ps1 files parse as curly quotes under CP1252 —
    scripts are pure ASCII now; keep them that way.
  - NOT yet runtime-tested end to end (the old launcher's VM was live all session);
    test on next VM restart: watchdog path, reboot-relaunch, poweroff reap, both
    display modes.

## Bare-metal validation (Windows laptop, 2026-08-27)

Hardware: AMD Ryzen 5 5625U (Zen 3, 6C/12T, AVX2), Radeon integrated GPU (Vulkan-capable
— Venus candidate), 16 GB RAM, Windows 11 Pro 26200.

Bootstrap status:
- [x] Repo cloned to `C:\cssi\try-omarchy-windows`
- [x] WHP optional feature enabled (needed one host reboot; hypervisor was already
      running via HvHost/vmcompute, but the HypervisorPlatform feature itself was off)
- [x] QEMU 11.1.0 installed via winget (qemu-w64-setup-20260811)
- [x] zstd 1.5.7 installed — **bootstrap.ps1 bug found+fixed:** winget id
      `Facebook.Zstandard` no longer exists, renamed `Meta.Zstandard`; also the
      `WinGet\Links\zstd.exe` shim didn't materialize in-session, so the script now
      falls back to searching WinGet Packages
- [x] v0.0.1-preview guest artifacts downloaded, rootfs decompressed (6 GiB)

**VALIDATED 2026-08-27 evening.** Results (details in FINDINGS.md):
- WHPX initialized first try; full boot → setup form → Hyprland desktop in the SDL
  window. Setup form driven 100% over QMP from the host (user brandon / hostname
  omarchy / America/Chicago).
- Clean provisioned boot: **3.22s kernel + 3.54s user = 6.8s to graphical.target**
  (vs 9.03s nested). ~40s launch→setup-splash on first boot incl. one-time disk copy.
- Second boot lands on SDDM login as expected — autologin/seamless-login remains the
  top UX gap for instant-try.
- **New trap:** in-guest `systemctl reboot` wedges QEMU (WHPX can't do system reset).
  `-no-reboot` added to launch-omarchy.ps1; app shell must relaunch on exit. See
  FINDINGS.md "NEW TRAP".
- llvmpipe subjective feel (Brandon, bare metal): desktop is workable, but media is
  not — YouTube video playback is poor and audio crackles (dsound + llvmpipe
  starving the audio thread on 4 vCPUs). Confirms Venus GPU path as the priority.
  To try at next relaunch: -smp 6, dsound latency= tuning or -audiodev sdl.
- SDL UX (see FINDINGS.md "SDL desktop UX findings"): cursor was invisible because
  the image's monitors.lua sets cursor invisible=true (correct for VNC hosts, wrong
  for SDL) — flipped to false in this VM, **must be fixed in the image builder**;
  the image uses Hyprland 0.56 Lua configs (edit *.lua + hyprctl reload; `hyprctl
  keyword` doesn't work); window resize tracks fine; Windows key fights the host
  Start menu unless input is grabbed (Ctrl+Alt+G) — app shell needs a keyboard hook.

## Current state

- Milestone reached 2026-08-27: full Omarchy desktop (Hyprland on llvmpipe) boots and
  renders under WHPX QEMU inside the dev VM. Parity with jorge-huxley's fork.
- Announced on X (quote-tweet of DHH, 2026-08-27), offering to collab. The stated
  differentiator vs jorge's fork: GPU acceleration via virtio-gpu Venus Vulkan
  (their spec self-declares llvmpipe-only), plus a slicker zero-setup UX.
- Read `docs/FINDINGS.md` for everything proven so far and the traps (especially
  the `-vga none` display trap and the WHPX CPU-model constraint).

## Environments

**Linux desktop (primary dev, this machine):** guest image builds (Docker), and a
dockur/windows Win11 VM used as the Windows test bed (nested KVM → WHPX works).
Fork checkout with the guest builder: `~/Projects/try-omarchy-win` (win branch).
Working scripts + built image shared into the VM at `~/Windows/tryomarchy/`.
Machine-specific access details (VM ssh, ports) live outside this repo.

**Windows laptop (BTS-CSSI-LAPTOP):** fully set up under both accounts (Brandon:
admin, original bootstrap; Tyler: non-admin, own guest image + git identity).
WINQ-EMU at C:\WINQ-EMU, stock QEMU 11.1.0 machine-wide. The running VM (user
tyler / pw omarchy) has everything from the image change list hand-installed —
use `-Fresh` when validating a new image so the stale disk doesn't mask bugs.

## Windows laptop bootstrap (the GPU/Venus milestone)

Goal: prove GPU-accelerated Omarchy via WINQ-EMU's Venus Vulkan path on real hardware.

1. Clone this repo. Read `docs/FINDINGS.md`.
2. Enable Windows Hypervisor Platform: elevated PowerShell,
   `Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All`, reboot.
   (Windows Home or Pro both fine.)
3. Install [WINQ-EMU Alpha 10](https://github.com/cmspam/winq-emu/releases) — exactly
   Alpha 10 if following Chainfire's recipe; check his repo
   (Chainfire/omarchy-windows-hyperv-gpu) for the interrupt patch and whether it
   applies (it targets Windows-on-Hyper-V hosts; a bare-metal laptop may not need it).
   Note WINQ-EMU wants BIOS boot, not EFI — our direct-kernel-boot image sidesteps that.
4. Get a guest image: build on the Linux box (`try-omarchy-win/guest/build-container.sh`,
   refresh the package lock first if Arch moved) and transfer `dist/guest/`
   (use rootfs.ext4.zst, ~1.2G, plus vmlinuz/initramfs/build-spec.json), or rebuild
   fresh if Docker is available.
5. Adapt `scripts/start-omarchy.ps1`: swap the QEMU path for WINQ-EMU's binary, replace
   `-vga none -device virtio-gpu-pci,id=gpu0` + VNC display with WINQ-EMU's
   virtio-gpu-gl/Venus device + SDL display config (see their launcher/docs), keep the
   rest of the recipe. First run headless smoke (`whpx-boot-test.ps1` pattern), then SDL.
6. Success criteria: Hyprland renders with `vulkaninfo`/`glxinfo` in-guest showing Venus
   (not llvmpipe), and animations/blur feel smooth. Grab numbers + screenshots, update
   this file and FINDINGS.md.

## TryOmarchy.exe + v0.0.3 image VALIDATED on real hardware (laptop, 2026-08-28 night)

Built app/ on the laptop (Go 1.27.0 zip toolchain, no admin, `go build -trimpath
-ldflags '-s -w -H windowsgui'` -> 7 MB exe) and ran the true first-run flow
against the live v0.0.3-preview release on a wiped data dir:

- First-run fetch: download + SHA256 + streaming zstd unpack of the 1.4 GB image
  in **66 seconds**, straight into GPU boot on attempt 1.
- The 10s-QMP-grace wedge fix held: attempt 1 healthy on every launch tonight
  (the PS launcher era regularly burned retries).
- **Lifecycle reboot-notify works on hardware**: `sudo reboot` -> shell.log
  "guest announced reboot" (port 4450) -> relaunch -> autologin desktop.
  Deterministic reboot-vs-poweroff, no RST race.
- v0.0.3 in-guest: 4.0.1-1, try-omarchy-reboot-notify.service enabled, 9p
  automount live, baked clipboard bridge connected on its own.
- verify-launch-ux.ps1: **all 9 checks PASS against the exe stack** (maximized,
  show-cursor, video=WxH, title, forwarder/supervisor/clipboard ports).
- Shell papercut found: launched from a background context, BOTH the download
  progress window and the SDL window open buried (no foreground rights) - the
  user saw nothing until raised by hand. A real double-click grants foreground,
  but the shell should SetForegroundWindow its progress window and the SDL
  window on first appearance anyway (AttachThreadInput trick if needed).
  Real-double-click first-run still to be confirmed by hand.
- Still user-verified pending: audio quality (dsound on hardware), winkey feel
  under the exe's in-process hook.

## v0.0.2-preview VALIDATED on real hardware (laptop, 2026-08-28 night)

Full HANDOFF checklist passed on the Ryzen 5 5625U with the real launcher
(fresh image + `-Fresh` disk, GPU/WINQ-EMU mode, user walked the form by hand):

- Silent boot: black window -> splash, zero console text ✓
- Cursor visible everywhere (see launch-UX contract: host cursor always on) ✓
- omarchy-version 4.0.1-1 ✓ · 22 themes ✓ · live theme switching
  (`omarchy theme set '<Name>'` — NOTE: 4.0.x replaced the omarchy-* scripts
  with the unified `omarchy` CLI; `omarchy-theme-next` etc. no longer exist) ✓
- Screensaver ✓ (auto-engaged on idle AND `omarchy screensaver`; the bare
  command runs inside the invoking terminal and closes it on exit)
- Clipboard both directions with ZERO setup ✓ (bridge auto-connects at login;
  it also makes a great sudo-free debug channel: `cmd | wl-copy` -> host
  Get-Clipboard — the serial tee needs sudo whose token expires every 5 min)
- `-Share` automounted at /mnt/host, no manual mount ✓
- Reboot -> supervisor relaunch -> straight to desktop, no greeter ✓
- **Clean second boot: 2.975s kernel + 3.104s user = 6.08s to graphical.target**
  (first boot's 2m33s userspace number is human-paced: the provisioning form
  blocks graphical.target while the user types)

Papercuts logged for image v3:
- Screensaver renders the compact logo instead of the spelled-out OMARCHY on
  small screens: the screensaver terminal has only 91 cols at 1366x768 —
  shrink its font on narrow screens so the full banner fits.
- Spare gettys on tty2-6 (raw `login:` via Ctrl+Alt+F2..6) — mask them.

Media: demo video (theme switching + screensaver) and theme/screensaver stills
in `C:\cssi\media-v002` on the laptop (captured host-side via
scripts/capture-window.ps1 + ffmpeg gdigrab; QMP screendump can't see the GL
path). Repo carries docs/media/launch-ux-reference.png.

## Launch-UX contract (settled 2026-08-28 on hands-on feedback — do NOT regress)

Enforced by `scripts/verify-launch-ux.ps1` (run it against a live VM; all PASS
required). Reference visual: docs/media/launch-ux-reference.png.

- Window opens **MAXIMIZED** — taskbar visible. Never fullscreen by default
  (user got trapped in fullscreen and hated it; `-Fullscreen` stays opt-in),
  never a small floating window.
- Guest console resolution = the maximized client area (`video=WxH` computed by
  the launcher from the work area), so the picture fills the window from the
  first frame; Hyprland re-adapts to live resizes after login.
- Host mouse cursor is **always visible** over the window (`show-cursor=on`) —
  during console phases the guest draws no cursor and a vanishing pointer reads
  as broken. Image-v3 consideration: with the host cursor always on, the guest
  cursor could go back to invisible (one cursor, host-drawn, VNC-era style) to
  avoid doubling on the desktop.
- Windowless QEMU binary, window titled "Try Omarchy", no console windows,
  forwarder + clipboard bridge + supervisor all connected.
- Setup form remains keyboard-only (inherent to the console form; same on
  macOS). Real fix on the roadmap: pre-provisioned "just try it" image.
- Spare guest gettys (tty2-6) reachable via Ctrl+Alt+F2..F6 show a raw
  `login:` prompt — image v3 should mask them (users must never see a console).

## Competitive: try-omarchy (macOS) v0.2.0 shipped 2026-08-28

Eduardo's release (github.com/themartiano/try-omarchy/releases/tag/v0.2.0):
Omarchy 4.0.1, folder sharing, clipboard sharing, package installs (ARM-compatible
only), ASCII animations/screensaver, default Omarchy window behavior. Our position
(direction: stay ahead; "98% native" with pragmatic trade-offs, image stays lean -
no 3GB+ downloads; additions so far total <20MB):

- Omarchy version: 4.0.1 as of v0.0.2-preview (was 4.0.0.alpha-1) - PARITY,
  plus all 22 themes vs the 6 the old image carried.
- Package installs: x86_64 = the entire Arch/AUR ecosystem works, vs his
  ARM-compatible subset. Structurally ahead, zero work needed.
- Screensavers: DONE in the dev VM (ttfx+hypridle); bake into image.
- Folder sharing: DONE 2026-08-28, launcher `-Share <folder>` via WINQ-EMU's
  virtio-9p (GPU mode; stock QEMU for Windows ships no 9p). Verified both
  directions on hardware (mount -t 9p -o trans=virtio hostshare /mnt/host).
  Remaining: auto-mount in guest (systemd.mount with nofail, image rebuild) and
  a default share folder in the UX.
- Clipboard sharing: DONE 2026-08-28, two-way, verified on hardware including
  across guest reboots. Own bridge, not vdagent (qemu-vdagent is absent from
  WINQ-EMU's build and X11-centric anyway): scripts/clipboard-bridge.ps1 on the
  host (started by the launcher; ports 4448 guest->host one-shot, 4449 host->guest
  persistent; base64 lines) + scripts/guest/clipboard-bridge.sh in the guest
  (wl-clipboard + socat to 10.0.2.2, systemd user service
  scripts/guest/clipboard-bridge.service, WantedBy=graphical-session.target -
  uwsm imports WAYLAND_DISPLAY so it Just Works). Compositor-native, works on
  both QEMU builds. Text-only v1; images/files later.
  Traps hit: (a) StreamWriter.WriteLine sends CRLF and base64 -d rejects the \r -
  set NewLine to LF; (b) do NOT put tr/sed between socat and the read loop - the
  extra pipe stage buffers ~4KB and small payloads never arrive; (c) see the
  FINDINGS note on the SHUTDOWN-event RST race the same session uncovered.
- Beyond parity (Windows-only wins already landed): Venus VULKAN (his stack is
  VirGL GL-only), focus-scoped Windows-key-as-Super, supervised lifecycle
  (launch-wedge retry, reboot relaunch, clean poweroff), zero QEMU chrome.

## Release plan — v0.0.2-preview

Goal (2026-08-28 direction): package this cleanly for non-technical Windows users
who want to try Linux without committing. Two tracks:

**Track 1 — script release (this repo, ready after end-to-end testing):**
- [x] Unified supervised launcher, no consoles, branded window, Win-key scoping
- [x] Non-admin-friendly bootstrap
- [x] End-to-end test DONE 2026-08-28 (on hardware): GPU boot + autologin;
      in-guest reboot -> auto relaunch; GPU poweroff -> clean exit; -NoGpu boot
      (visible cursor, autologin, screendump OK); stock poweroff wedge -> probed
      and reaped; no console windows; window stays titled "Try Omarchy".
      Two bugs found+fixed by testing: wedged QEMU never delivers its SHUTDOWN
      event (supervisor now probes liveness), and the forwarder didn't match the
      w-binary process name. Untested still: -Fresh, first-boot-form path,
      launch-wedge retry (no wedge occurred post-fix; logic exercised in review
      only).
- [x] Folder sharing via `-Share` (see competitive section)
- [ ] Tag v0.0.2-preview reusing the v0.0.1 guest artifacts

**Track 2 — image rebuild: DONE 2026-08-28 evening, shipped as v0.0.2-preview**
(see the session log near the top of this file for the details):
- [x] Omarchy pin bumped to 4.0.1 (+ all 22 themes; omarchy-version reports 4.0.1-1)
- [x] ttfx, hypridle, vulkan-virtio added (wl-clipboard was already in)
- [x] Clipboard bridge baked in (user unit enabled globally)
- [x] 9p auto-mount (condition-guarded mnt-host.mount)
- [x] Cursor visible under SDL
- [x] Autologin permanent (upstream writes it; our drop-in disarms its one-boot
      cleanup — the generic SDDM greeter never appears)
- [x] Boot console text hidden — launcher-side cmdline change, no plymouth
      (plymouth is NOT in the image; the old "it ships" note was wrong)
- [x] Lock refreshed, image rebuilt, KVM-validated, published, bootstrap bumped
- Stretch (still open): pre-provisioned "just try it" variant (no setup form),
  dev variant with sshd + hostfwd

## Open work, in rough order

**Road to the announcement (as of 2026-08-28 late night):**
- [x] 2026-08-28: TryOmarchy.exe rebuilt from master (Go 1.27, 7.6 MB, PE32+
      GUI) and attached to v0.0.3-preview with TryOmarchy.exe.sha256;
      tryomarchy.com/download and /TryOmarchy.exe redirect to the latest
      release asset (Cloudflare _redirects). Decision: bootstrap.ps1 stays as
      the required one-time setup (WHP enable + QEMU install need admin; the
      exe checks and refuses with a pointer if they're missing) - site and
      README now serve it clone-free via tryomarchy.com/bootstrap.ps1.
- [ ] Sign the exe (docs/SIGNING.md + scripts/sign.ps1 are wired to the Azure
      Trusted Signing account) so SmartScreen doesn't scare non-tech users.
- [ ] Image v3 papercuts: screensaver terminal font on narrow screens (91 cols
      at 1366x768 renders the compact logo, not the full wordmark), mask the
      spare tty2-6 gettys, fold guest-build patch 0003 (clipboard socat
      silence) into the shipped image.
- [x] 2026-08-28: tryomarchy.com live (Cloudflare Pages, git-connected repo
      tsouth89/tryomarchy-site): real capture loop in the hero, exe download
      flow, honest SmartScreen note, credits and unofficial disclosure.
- [ ] Announcement post: numbers to quote - 6.08s boot, 66s setup download,
      Venus Vulkan, 22 themes, clipboard + folder sharing, 7 MB app.

- [x] CPU-feature experiment DONE 2026-08-27: XSAVE/AVX panics guests under upstream
      WHPX; safe ceiling `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes` (now default in
      scripts/start-omarchy.ps1). AVX2 needs WINQ-EMU's patched WHPX. See FINDINGS.md.
- [x] Boot profile baseline 2026-08-27: 9.03s to graphical.target in the nested dev
      VM (5.1s kernel + 4.0s user). Trim + SDDM autologin still open.
- [~] Seamless/auto login: PROVEN 2026-08-27 on the laptop VM — `/etc/sddm.conf.d/
      autologin.conf` with `[Autologin] User=brandon Session=hyprland-uwsm` boots
      straight into Hyprland (Brandon: user should never see the SDDM screen).
      Remaining: make provisioning write this conf for the created user, and bake
      it into the pre-provisioned "just try it" image variant.
- [ ] "Just try it" mode: auto-provision a default account over QMP (or pre-provisioned
      image variant) so first boot lands straight in Hyprland.
- [ ] Dev-image variant with openssh + QEMU hostfwd for real in-guest automation
      (current image is 79 packages, no sshd; everything goes through QMP send-key).
- [x] Venus/WINQ-EMU on the laptop DONE 2026-08-27: WINQ-EMU Alpha 10 installed
      (C:\WINQ-EMU), scripts/launch-omarchy-gpu.ps1 merges their stack with our
      direct-kernel boot. Hyprland renders via virgl on the Radeon iGPU; vulkaninfo
      shows Venus; -cpu host works. See FINDINGS.md "VENUS MILESTONE HIT".
      Verdict (Brandon, hands-on): YouTube audio and video both look/sound great
      on the GPU build — the llvmpipe-era crackle and stutter are gone
      (virtio-sound + virgl/Venus on the Radeon iGPU).
      Follow-ups: add vulkan-virtio (+vulkan-tools) to the image package set;
      benchmarks and screenshots for the announcement; consider WINQ-EMU's
      virtio-9p for host file sharing.
- Screensaver broken in the trimmed image: omarchy screensaver scripts ship but
  their deps don't — **ttfx** (in the image's own omarchy pacman repo!) and
  **hypridle** (extra). Installed in the running guest, works (Brandon-verified;
  its exit also restores the cursor). **Image package set must add: ttfx,
  hypridle** — Brandon: screensavers are a big part of Omarchy's appeal, treat as
  required, not optional. (foot/socat/jq were already present.)
- SDL grab trap: with Ctrl+Alt+G grab active, WINQ-EMU's bundled SDL suppresses
  the Windows key system-wide even when the QEMU window is unfocused (Start menu
  and Win+Shift+S dead until released); auto-grab re-engages on mouse-over (stock
  QEMU absolute-pointer behavior), so releasing doesn't stick. FIXED with the
  first app-shell component: scripts/winkey-forwarder.ps1 — SDL keyboard grab
  disabled (SDL_GRAB_KEYBOARD=0), and a focus-scoped WH_KEYBOARD_LL hook swallows
  Win only while the QEMU window is foreground, forwarding it to the guest as
  meta_l over a second QMP socket (4446). launch-omarchy-gpu.ps1 starts/stops it
  automatically. (SDL2 2.32.10 in both QEMU builds, so this wasn't a stale-SDL
  bug — grab-scoping is on us.)
- [ ] Native app shell design: window embedding vs own display client (QMP/VNC/D3D
      surface), lifecycle, sparse-disk management, WHP-enable installer flow.
- [ ] Reach out to Eduardo (themartiano) and Jorge (jorge-huxley) re: collab, and
      consider upstreaming the `-vga none` finding to jorge's fork.

## Done

- 2026-08-27: WHPX proven in dev VM (Alpine boot), Omarchy x86_64 image built (lock
  refresh needed), full provisioning driven over QMP, Hyprland rendering confirmed.
  Repo created, scripts and findings captured.
