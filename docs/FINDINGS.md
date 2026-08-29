# Technical findings

Working notes on what's proven, what bit us, and the fixes. Dates are 2026-08.

## WHPX boot recipe (proven)

```
qemu-system-x86_64 -accel whpx -machine q35 -cpu qemu64 -smp 4 -m 4096
  -drive file=disk.raw,format=raw,if=virtio
  -kernel vmlinuz-linux -initrd initramfs-linux.img -append "<cmdline>"
  -vga none -device virtio-gpu-pci,id=gpu0
  -device virtio-keyboard-pci -device virtio-tablet-pci
  -device virtio-net-pci,netdev=n0 -netdev user,id=n0
  -device virtio-rng-pci
  -display vnc=127.0.0.1:7
  -qmp tcp:127.0.0.1:4445,server=on,wait=off
  -serial file:serial.log
```

- `-cpu qemu64`, not `-cpu host`: upstream QEMU's WHPX backend rejects host passthrough (WINQ-EMU patches this). qemu64 is a 2006-era feature set — no AVX2, which hurts llvmpipe badly. Testing richer named models / feature flags is an open task.
- QEMU 11 matters: Chainfire documented WHPX interrupt fixes landing in QEMU 11 (and a startup regression on Hyper-V-host setups that WINQ-EMU Alpha 10 + his patch work around). Stock QEMU 11.1.0 from winget works fine under WHPX-on-KVM-nested and should work on bare-metal Windows.
- Direct kernel boot (no bootloader) + raw ext4 rootfs on virtio-blk, from the try-omarchy build system.
- Writable disk on NTFS: copy rootfs.ext4 to disk.raw, `fsutil sparse setflag`, then extend to the spec's expanded size via SetLength. Allocate-on-write; hosts without 24 GiB spare still boot. (Credit: jorge-huxley.)

## THE display trap: default VGA + virtio-gpu (cost us an hour)

`-device virtio-gpu-pci` does **not** suppress QEMU's default VGA. You silently get two display devices. The guest kernel's fbcon/DRM moves to the virtio-gpu, while QMP `screendump` (and anything watching the default console) keeps showing the **stale VGA text buffer** — frozen at early boot messages. It looks exactly like a boot hang. Keystrokes go through and land on the invisible display.

Fix: always pass `-vga none` alongside `-device virtio-gpu-pci`. Prefer `-display vnc=127.0.0.1:N` over `-display none` for headless work so the display pipeline stays live.

Any embedded/headless display client in the app must account for this.

## First-boot provisioning

The factory image arms upstream Omarchy's `omarchy-provision-owner.service`: an interactive gum form on tty1 (keyboard, username, password, optional name/email, hostname, timezone), run before display-manager. Completing it creates the user and hands off to SDDM → Hyprland.

The entire form is drivable over QMP `send-key` (see `scripts/qmp.ps1` — `type`/`key`/`shot` ops). This is the basis for an app-driven setup UX, or a "just try it" mode that provisions a default account with no questions.

## Guest image build

Built with the containerized x86_64 builder from jorge-huxley/try-omarchy-win (`guest/build-container.sh`, Docker on Linux, ~10 min). Two operational notes:

- The build enforces `packages.lock.json` against live Arch repos and refuses on drift. Refresh with `guest/build-container.sh --refresh-package-lock guest/packages.lock.json`, review the diff, rebuild. Expect this routinely — Arch moves fast.
- The build spec self-documents the graphics state: `guestRenderer: llvmpipe, hostRenderer: none`. The trimmed 79-package guest has no sshd; all in-guest automation goes through QMP keystrokes. A dev-image variant with openssh (+ QEMU `hostfwd`) would make benchmarking much nicer.

## Dev environment: nested virtualization under dockur/windows

Development runs inside a dockur/windows Win11 VM on a Linux/KVM host (so: Linux → KVM → Windows 11 → WHPX → Omarchy). Findings:

- dockur **masks VMX by default for Windows guests** (`-cpu ...,-vmx`, guarding an old Windows-update crash). Set the `VMX: "Y"` env on the container to expose nested virt; current Win11 builds run fine with it.
- After exposing VMX, enable the `HypervisorPlatform` optional feature in the Windows guest + reboot; then `-accel whpx` initializes. ("WHPX: No accelerator found" = the hypervisor isn't launching — check VMX exposure first.)
- The same enable-WHP + reboot flow is what the product installer must automate on end-user machines.

Caveat: all performance numbers measured in this nested environment carry KVM-nesting overhead. Relative comparisons (flag A vs flag B) are valid; absolute UX judgments need bare-metal Windows.

## GPU path (planned, needs real hardware)

WINQ-EMU proves Venus Vulkan forwarding works on Windows QEMU (their benchmark: 410 fps SuperTuxKart vs 226 under WSL2), with virgl for GL and experimental DXVA video decode. Their caveat: BIOS boot, not EFI (EFI boot tanks Vulkan perf). The dev VM has no GPU to forward, so this work needs a physical Windows machine.

## WHPX CPU features: the XSAVE cliff (2026-08-27 evening)

Upstream QEMU WHPX accepts any `-cpu` model at launch, but **any XSAVE-state feature
(AVX and above) panics the guest kernel at ~0.25s in `fpstate_reset`** — upstream WHPX
does not virtualize XSAVE/XCR0 state correctly. Launch acceptance means nothing;
Skylake-Client-v3, Haswell-v4, and `qemu64,+avx,...` all pass validation and then
panic the guest.

- Safe proven ceiling: `qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes` — boots the full
  Omarchy image cleanly, flags visible in-guest. llvmpipe benefits from SSE4.1/4.2,
  so ship this as the fallback-tier CPU model instead of bare qemu64.
- AVX2-accelerated llvmpipe therefore REQUIRES WINQ-EMU's patched WHPX (their
  "full -cpu host passthrough" claim is exactly this fix). WINQ-EMU matters for the
  CPU-rendering fallback, not just Venus.
- Methodology traps, learned the hard way: (1) a panicked guest leaves the QEMU
  process running — check guest health, not process liveness; (2) Windows OpenSSH
  kills the session's process tree on disconnect, so QEMU must not outlive the ssh
  session that started it (hold the session or use a scheduled task); (3) Alpine's
  `virt` kernel has no virtio-gpu driver — use `-device VGA` for Alpine-based text
  harnesses; the Omarchy image has virtio-gpu and is fine.

## WHPX needs the hypervisor, not the feature (2026-08-28, empirical)

Proven on the laptop while trying to fake a factory-fresh machine: with the
HypervisorPlatform optional feature **Disabled** (dism-confirmed) AND
`hypervisorlaunchtype off`, WHPX still fully works — `WHvGetCapability`
reports the hypervisor present and QEMU boots the guest normally — because
**VBS/HVCI (Memory Integrity) relaunches the hypervisor regardless**, and
WinHvPlatform.dll ships with the OS. Consequences:

- On machines with Memory Integrity on — the DEFAULT on new Windows 11
  machines — Try Omarchy needs **no setup at all**: no feature enable, no UAC,
  no restart. Download, open, desktop. The WHP-enable flow is a fallback for
  machines with VBS off and no WSL2/Hyper-V.
- The app's functional probe (WHvGetCapability, not feature state) is the
  right check: feature state and WHPX availability genuinely diverge.
- A dev machine with VBS or WSL2 cannot simulate a bare machine by disabling
  the feature or even `hypervisorlaunchtype` — Memory Integrity must be off
  too. Test the enable flow in a VM instead.

## Bare-metal validation (2026-08-27, Ryzen 5 5625U laptop, Windows 11 Pro)

The v0.0.1-preview flow works on real hardware. WHPX initialized first try (QEMU
11.1.0 prints a benign `warning: Ignoring request for interrupt vector 0` at start).
First boot: ~40s from launch to the setup splash (includes the one-time 6 GiB sparse
disk copy); the entire gum form was driven over QMP from the host — the provisioning
automation works on bare metal exactly as in the dev VM. Second (provisioned) boot:
**3.22s kernel + 3.54s userspace = 6.8s to graphical.target** (vs 9.03s nested), then
SDDM login → full desktop with wallpaper. SDL display worked throughout (screendumps
via QMP match what the window shows; no SDL-specific issues observed).

### SDL desktop UX findings (bare metal)

- **Invisible mouse cursor — an image config decision, not a QEMU bug:** the guest
  image's `~/.config/hypr/monitors.lua` sets `hl.config({ cursor = { invisible =
  true } })` ("hide the guest cursor because the host composites it outside the
  guest"). True under VNC (the dev-VM setup: the viewer draws a local cursor), false
  under SDL — nothing draws any cursor. Fix:
  `sed -i 's/invisible = true/invisible = false/' ~/.config/hypr/monitors.lua` +
  `hyprctl reload`; the cursor then renders in-frame and SDL shows it. **Fix in the
  guest image builder: cursor must be visible whenever the display is SDL.**
- **This image uses Hyprland's Lua config** (Hyprland 0.56.2, `~/.config/hypr/*.lua`
  + base `/usr/share/hypr/hyprland.lua`). `hyprctl keyword` is rejected ("can't work
  with non-legacy parsers") and a classic `hyprland.conf` is ignored entirely — edit
  the Lua files and `hyprctl reload`. Input/mouse injection over QMP works fine
  (`input-send-event` with abs coords, 0–32767 scale).
- **Window resize works:** virtio-gpu propagates SDL window resizes; Hyprland adapts
  its resolution live. Transient cropping can appear until the next resize event.
- **Windows key collision:** Super is Omarchy's main modifier, but the host swallows
  it (Start menu opens) unless QEMU has grabbed input. Ctrl+Alt+G (SDL grab) makes
  SDL install the low-level keyboard hook that swallows Win-key on the host;
  fullscreen also helps. **And the reverse trap:** with the grab active, the bundled
  SDL (observed with WINQ-EMU Alpha 10's SDL2.dll) keeps suppressing the Windows key
  even when the QEMU window is NOT focused — Start menu and Win+Shift+S (Snipping
  Tool) die system-wide until the grab is released with Ctrl+Alt+G. App-shell
  requirement: the keyboard hook must be strictly scoped to window focus — swallow
  Super only while the guest window is foreground, pass it through otherwise.

### NEW TRAP: in-guest reboot wedges QEMU under WHPX

`systemctl reboot` inside the guest hangs the whole VM at the reset: the guest shuts
down cleanly, issues the reset, and never comes back — SDL window freezes on black,
serial stops, QMP stops answering, and the QEMU process sits alive at ~0% CPU.
Upstream WHPX apparently cannot execute a system reset (same class of gap as the
XSAVE cliff). Also observed: after force-killing the wedged process, the *next*
launch froze at ~1.4s into kernel boot (possibly leaked WHPX partition state);
killing that one and launching again booted clean in ~20s.

Mitigation: launch with `-no-reboot` so a guest-initiated reset exits QEMU instead of
wedging it (now in launch-omarchy.ps1). The app shell must treat guest reboot as
"QEMU exits → relaunch", e.g. via `-action reboot=shutdown` + QMP RESET/SHUTDOWN
events if it needs to distinguish reboot from poweroff.

## VENUS MILESTONE HIT (2026-08-27, bare-metal laptop, WINQ-EMU Alpha 10)

`scripts/launch-omarchy-gpu.ps1` = our direct-kernel-boot recipe + WINQ-EMU's
graphics stack (binary at `C:\WINQ-EMU\bin\qemu-system-x86_64.exe`, QEMU 11.0 +
their patch series, virglrenderer 1.3.0). Verified on the Ryzen 5 5625U / Radeon
iGPU:

- **`-cpu host` boots clean** under their patched WHPX (the XSAVE cliff is fixed
  there) — ~19s to login prompt, full host feature set including AVX2.
- **Hyprland renders on the real GPU via virgl**: Aquamarine logs
  `Renderer: virgl (AMD Radeon (TM) Graphics)`, OpenGL ES 3.2 Mesa 26.2.1.
- **Venus Vulkan confirmed**: `vulkaninfo` reports
  `Virtio-GPU Venus (AMD Radeon (TM) Graphics)`, `driverName = venus`.
  BUT the guest image ships no Venus ICD — had to `pacman -S vulkan-virtio
  vulkan-tools` in the running guest (network works). **Add `vulkan-virtio` to the
  image package set** (and consider `vulkan-tools` for the dev variant).
- Device recipe: `-device virtio-vga-gl,blob=on,hostmem=4G,venus=on -display
  sdl,gl=on` (per WINQ-EMU's launcher; virtio-vga-gl IS the VGA device — no `-vga
  none`, and no two-display trap observed). Direct kernel boot sidesteps their
  BIOS-vs-EFI warning entirely.
- Audio switched to `-device virtio-sound-pci` (their recipe; dsound/intel-hda
  crackled badly under llvmpipe load).
- **QMP `screendump` does not work on the GL path** — returns `"no surface"`
  (scanout lives in a GPU texture with blob=on). Headless monitoring must use the
  serial console instead; trick: run `<cmd> | sudo tee /dev/ttyS0` in-guest and the
  output lands in the host-side serial log file. A future dev image should add
  sshd + hostfwd (or use WINQ-EMU's virtio-9p sharing).

Also learned: guest-initiated **poweroff** wedges upstream/stock WHPX QEMU exactly
like reboot does (QMP `system_powerdown` → guest shuts down → QEMU hangs at the
final ACPI transition, ~0% CPU, must force-kill). The reboot trap section applies
to every guest-initiated reset/poweroff on stock QEMU 11.1.0.

**Corollary trap #2 (2026-08-28): a fast QEMU exit can DISCARD its SHUTDOWN event.**
With `-no-reboot`, QEMU exits almost immediately after a guest reset; the abortive
socket close (RST) throws away unread data in the peer's receive buffer, so a
supervisor that sleeps-then-reads sometimes never sees the SHUTDOWN event and
cannot tell reboot from poweroff. Fix in launch-omarchy.ps1: keep an async
ReadLine permanently pending on the QMP socket - the event is consumed the moment
it arrives, before the RST can eat it.

**Corollary trap (2026-08-28): the poweroff wedge can LOSE recent guest writes.**
A file written ~20s before `sudo poweroff` (autologin.conf, confirmed written via
tee output) was gone on the next boot — its parent mkdir survived, the file didn't.
The wedge evidently hits before the final filesystem flush completes, and the
force-kill discards whatever the guest still had in flight. Rule: run `sync` after
any write that matters, and don't treat a wedged-then-killed poweroff as a clean
shutdown. (WINQ-EMU's build exits cleanly on guest reboot with `-no-reboot` —
no wedge observed there.)

## Boot profile (nested dev VM, SSE4.2 pack)

`systemd-analyze` in the Omarchy guest: **5.06s kernel + 3.97s userspace = 9.03s to
graphical.target**, then SDDM. Top blame: dev-vda.device 1.66s,
systemd-tmpfiles-setup-dev-early 1.14s, user-runtime-dir 1.10s, systemd-userdbd 1.06s.
Bare metal will be faster (this carries KVM-nesting overhead). Note: after first-boot
provisioning auto-login, subsequent boots land on SDDM login — a seamless-login or
autologin config is needed for the "instant try" UX.

## THE LAUNCH WEDGE, SOLVED: early QMP connections deadlock WHPX QEMU (2026-08-28 night)

The "launch wedge" (QEMU alive, QMP accepts TCP but the main loop never answers,
window Not Responding) is not random: **connecting to a QMP socket during the
guest's first seconds of boot is what triggers it.** Reproduced in the nested dev
VM at effectively 100% - eight shell launches in a row wedged with a supervisor
probing QMP from t=1.5s, while the identical QEMU invocation left untouched for
30s answered the handshake instantly, every time. On bare metal the same race
exists but the window is smaller (faster boot), which is why the laptop saw it
"sometimes, twice in a row" instead of always.

Rule: never touch any QMP socket before the guest is past early boot. The app
shell (app/main.go) waits 10s before the supervisor's first probe, and the
winkey forwarder + tooling only connect after that handshake succeeds. With the
delay in place, nested launches come up healthy on attempt 1 in ~11s.

Two more shell-era findings the same night:

- **virtio-sound-pci without an explicit -audiodev hangs the whole guest
  session.** The device answers nothing; PipeWire blocks on virtio-snd control
  messages (`control message (0x00000102) timeout` on serial) and the desktop
  freezes black. Always pass `-audiodev dsound,id=snd` +
  `virtio-sound-pci,audiodev=snd`. On machines with no DirectSound device at
  all (VMs, some remote sessions) QEMU then exits at startup - the shell
  detects that and relaunches with `-audiodev none` so the app still works,
  just silent. (The unified launch-omarchy.ps1 shipped this bug; real laptops
  masked it because QEMU's default backend happened to work there.)
- **A reboot-wedged QEMU also loses the serial file's final flush**, so the
  kernel's "reboot: Restarting system" line cannot be sniffed to tell reboot
  from poweroff after the fact. The image now closes this properly: a shutdown
  unit (try-omarchy-reboot-notify, guest-build patch 0004) reports reboot
  intent to the shell on lifecycle port 4450 while the network is still up.
