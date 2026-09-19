# Laptop pass: camera, direct drops, sleep, and validation

One sitting to exercise the features that are implemented but not yet verified on
real Windows, plus the hardware checks v1 still needs. Record results with
[TESTING.md](TESTING.md), including the launcher and guest hashes and the Windows
build and GPU.

## Build under test

The earlier v20 guest assets are obsolete. The corrected candidate was built
from `e402965` on `codex/preview20-reliability`, with guest compatibility **28**.
Camera, browser microphone and clean file drops passed on the physical laptop;
the user confirmed the repeated drag. The full rebuilt guest passed CI build
and fresh-boot smoke. Exact packaged and signed acceptance remains in progress.
See the [drop evidence](evidence/FILE-DROP-2026-09-19.md).

Use one candidate directory containing the verified launcher and complete guest
and runtime assets. Record the commit and SHA256 of the launcher, `SHA256SUMS`,
`vmlinuz-linux`, `initramfs-linux.img` and runtime ZIP before testing. Confirm
Authenticode is valid for signed-candidate acceptance. An unsigned CI build is
useful for diagnosis only.

## Start an isolated candidate

Copy the existing installation into a separate directory first. Keep the original
stopped and intact. Serve the candidate assets with:

```powershell
py -m http.server 18080 --bind 127.0.0.1 --directory C:\TryOmarchyCandidateAssets
```

In another PowerShell window, use the hash from those same assets:

```powershell
$assets = 'C:\TryOmarchyCandidateAssets'
$sumsHash = (Get-FileHash "$assets\SHA256SUMS" -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateArgs = @(
  '-dir', 'C:\TryOmarchyCandidateTest',
  '-release', 'http://127.0.0.1:18080', '-sums-sha256', $sumsHash,
  '-runtime-release', 'http://127.0.0.1:18080', '-runtime-sums-sha256', $sumsHash,
  '-no-update'
)
& "$assets\TryOmarchy.exe" @candidateArgs
```

Repeat with an empty data directory for fresh-install acceptance. Check the
running QEMU path and hash to rule out an old `C:\WINQ-EMU` override.

## 1. Camera

Test the channel first without hardware, then the real camera.

```powershell
$env:TRYOMARCHY_FAKE_CAMERA = "1"   # synthetic frames
& "$assets\TryOmarchy.exe" @candidateArgs
```

Inside Omarchy:

```sh
ls -l /dev/video42
journalctl --user -u omarchy-windows-camera-bridge -b
mpv av://v4l2:/dev/video42        # or any camera app; only run while testing
```

- Expect: `/dev/video42` exists, the bridge logs the port and a start/stop cycle,
  and the app shows a moving bar.
- Then run `Remove-Item Env:TRYOMARCHY_FAKE_CAMERA`, relaunch, and repeat with the real camera.
- Expect: the app shows the real camera, and **the Windows camera indicator lights
  only while an app is using it** (on-demand), not while idle.
- Watch the launcher log for `camera:` lines and Media Foundation errors.
- First failure to expect: `0x80070005` (access denied) means Windows camera access
  is off for desktop apps; `no camera was found` means enumeration or privacy.

Record: whether `/dev/video42` appears, the bridge's start/stop behavior, whether
frames arrive, indicator behavior, and any error codes.

## 2. Direct drops

1. Open a local folder in Files and drop a Windows file into its empty area.
   Expect the file to appear directly, without a transfer window or clipboard change.
2. Repeat the same file. Expect a numbered copy; the original must remain intact.
3. Drop outside a recognized Files folder. Expect delivery to Downloads and a small
   notification. Ambiguous tabs/windows, other apps and nonlocal folders use this
   fallback; arbitrary application imports and nested folder-icon targeting are
   not supported.
4. For a slower transfer, close Windows progress. Expect copying to continue.
   Its Cancel button should stop copying without changing the Windows originals.
5. Drop while the guest is still starting. Expect a clear message and no lost file.

Record destination and source hashes, absence of an automatic transfer window,
and whether progress stays responsive.

## Camera and audio lifecycle

Close and reopen capture at least three times, then try a browser call. Confirm
real moving frames, microphone signal from speech, playback, and camera indicator
shutdown after closing the client. Repeat after sleep and after switching audio
devices. A black frame or nonzero recording byte count alone is not a pass.

On an upgraded disk, check `sudo modprobe tun`, `/dev/net/tun`,
`sudo modprobe v4l2loopback`, and `/dev/video42` before running any package update.
Verify the compatibility marker is `28:<running kernel>` and the next boot does
not repeat module delivery. Preserve fixture hashes through upgrade and rollback.

## 3. Pause on host sleep

1. With Omarchy running, sleep Windows, wait, wake.
2. Expect: the guest resumes, the clock is right, timers and animations are not
   stuck, and no window redraw loop starts.
3. Check the launcher log for `power: paused the guest` and `power: resumed`.

Record: resume time, clock correctness, and any stuck UI or audio.

## 4. LAN forwarding

Settings → **Add LAN…**, choose a Windows adapter, forward a guest service (for
example TCP 8080 to guest 80). From a second device on the same LAN, connect to
`http://<windows-lan-ip>:8080`. Expect the firewall rule to be created for the
chosen adapter only. Private/domain networks work by default; public requires the
Settings toggle.

Record: reachability, firewall prompt, and behavior after the adapter changes.

## 5. Portable mode

On a USB drive: launch with `-portable`, let it create the layout, boot, use it,
power off, then change the drive letter and boot again. If a second PC is
available, move the drive there.

Record: boot after a letter change, second-PC boot, and files surviving.

## 6. Multiple displays and mixed DPI

With two monitors at different scale factors, move the Omarchy window between
them and fullscreen on each.

Record: guest resolution and Hyprland scale after each move, and whether the
cursor or input lands where expected.

## Safety and limits

- Back up before this pass; it runs unreleased guest code.
- The camera bridge and direct drops are not in any published release.
- Accelerated RAM resume is intentionally not included; pause-on-sleep is.
- Bridged networking (guest on the LAN with its own address) is not included.
