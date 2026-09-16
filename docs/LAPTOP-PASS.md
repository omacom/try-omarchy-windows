# Laptop pass: camera, direct drops, sleep, and validation

One sitting to exercise the features that are implemented but not yet verified on
real Windows, plus the hardware checks v1 still needs. Record results with
[TESTING.md](TESTING.md), including the launcher and guest hashes and the Windows
build and GPU.

## Build under test

A **draft release**, `v0.0.20-preview`, holds everything: the launcher, the guest
artifacts, and both runtime archives. It is unpublished, so the published `Latest`
remains v0.0.19-preview.

| Piece | Value |
| --- | --- |
| Release | `v0.0.20-preview` (draft), target `f8e9705` |
| Launcher | `TryOmarchy.exe`, sha256 `874915f399a3411b1611e5b3fda5d1f4f4dfb31aa40b453d79d99d24e1ce8998` (unsigned; SmartScreen may warn) |
| Guest | compatibility revision **26** (camera bridge, direct drops) |
| `SHA256SUMS` | sha256 `f7333157627beaeae356271f48377390c25d1046f147f29e6a269c021ad699db` |

## Get it on the laptop and run it

1. Download the draft (needs GitHub auth; a draft is not public):

   ```powershell
   gh release download v0.0.20-preview --repo omacom/try-omarchy-windows --dir C:\TryOmarchyDraft
   ```

2. Serve that folder over loopback (a draft's asset URLs need a token, so the
   launcher cannot fetch them directly):

   ```powershell
   cd C:\TryOmarchyDraft
   py -m http.server 18080 --bind 127.0.0.1
   ```

3. **Use a copy, never your only install.** Copy `%LOCALAPPDATA%\TryOmarchy`
   somewhere else, or point the launcher at a fresh data directory.

4. Start the candidate:

   ```powershell
   C:\TryOmarchyDraft\TryOmarchy.exe `
     -dir C:\TryOmarchyTest `
     -release http://127.0.0.1:18080 `
     -sums-sha256 f7333157627beaeae356271f48377390c25d1046f147f29e6a269c021ad699db `
     -runtime-release http://127.0.0.1:18080 `
     -runtime-sums-sha256 f7333157627beaeae356271f48377390c25d1046f147f29e6a269c021ad699db `
     -no-update
   ```

   `-no-update` keeps the launcher from replacing itself with the published pin.
   Keep the `py -m http.server` window open until Omarchy is running.

## 1. Camera

Test the channel first without hardware, then the real camera.

```powershell
$env:TRYOMARCHY_FAKE_CAMERA = "1"   # synthetic frames
.\TryOmarchy.exe -dir C:\TryOmarchyTest -release http://127.0.0.1:18080 -sums-sha256 fee9d06a... -runtime-release http://127.0.0.1:18080 -runtime-sums-sha256 fee9d06a... -no-update
```

Inside Omarchy:

```sh
ls -l /dev/video42
journalctl --user -u omarchy-windows-camera-bridge -b
mpv av://v4l2:/dev/video42        # or any camera app; only run while testing
```

- Expect: `/dev/video42` exists, the bridge logs the port and a start/stop cycle,
  and the app shows a moving bar.
- Then remove `TRYOMARCHY_FAKE_CAMERA`, relaunch, and repeat with the real camera.
- Expect: the app shows the real camera, and **the Windows camera indicator lights
  only while an app is using it** (on-demand), not while idle.
- Watch the launcher log for `camera:` lines and Media Foundation errors.
- First failure to expect: `0x80070005` (access denied) means Windows camera access
  is off for desktop apps; `no camera was found` means enumeration or privacy.

Record: whether `/dev/video42` appears, the bridge's start/stop behavior, whether
frames arrive, indicator behavior, and any error codes.

## 2. Direct drops

This is the one place the guest-side targeting is unproven, so try both a hit and
a miss.

1. In Omarchy, open Files (or an editor) and note roughly where its window is.
2. Drag a file from Explorer onto the VM window, over that app window.
   - Expect: the app receives the file (import/paste); **no transfer window appears**.
3. Drop onto the desktop or an empty area with no app window.
   - Expect: the transfer window appears (the fallback), and the file is available.
4. Drop while the guest is still starting.
   - Expect: a clear message, no lost file.

Record: whether targeting hit the right window, the app's behavior, and fallback.

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
