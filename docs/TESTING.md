# Testing a Windows release candidate

Use a separate data folder and a copy of any existing guest. Keep the original
backup untouched. Follow [RELEASING.md](RELEASING.md) for running a signed draft
candidate against locally served, authenticated assets.

## Report details

Copy this into #10 or the relevant issue. Mark untested items as untested.

```text
Launcher version and SHA256:
Runtime archive SHA256:
Guest release; fresh install or upgraded from:
Windows edition, version, and build:
CPU:
GPU and driver version:
RAM:
Physical machine or nested VM:
Full Hyper-V enabled; WSL2 installed:
Data filesystem and drive free space:
GPU rendering or CPU fallback:
Results and steps for any failure:
```

Use diagnostics from the tray or `TryOmarchy.exe -diagnostics` for failures.
Review the zip before attaching it; logs can contain local paths and other
personal details. Do not upload the guest disk or a full backup.

## Everyday use

- [ ] Fresh install at the default location and another local drive. Use the
  Windows folder picker for install location and sharing, including a long path.
- [ ] On a copied install, confirm that malformed preferences offer repair,
  declining leaves them unchanged, and accepting preserves the original file
  before restoring defaults without changing guest files.
- [ ] Cancel a download, relaunch, and finish setup without a broken install.
- [ ] Instant account and personalized account both reach the desktop.
- [ ] GPU rendering, then CPU fallback, both reach the desktop.
- [ ] Keyboard and Windows key work only in the intended window; host shortcuts
  still work after switching away. Windows handles Win+L itself.
- [ ] Text clipboard works in both directions, including Unicode, trailing
  newlines, copying an earlier value again, and reconnecting after a guest reboot.
- [ ] Shared files can be read and written from both sides.
- [ ] Audio playback, device switching, and video work. Note microphone activity
  at idle and when an app starts and stops recording.
- [ ] Resize, fullscreen, and movement between monitors with different scaling.
- [ ] Guest reboot, poweroff, relaunch, Windows sleep, and resume.
- [ ] At least one hour of use without the launcher exiting or input forwarding
  stopping. Report idle CPU after five minutes without animation.
- [ ] RDP and VNC sessions, including Ctrl+Alt+G as the input fallback.
- [ ] Image clipboard: a Windows screenshot pastes into an Omarchy app, and an
  image copied in Omarchy pastes into a Windows app. Text keeps working after.
- [ ] The guest clock matches Windows after a sleep and resume, and the desktop
  keyboard layout and time zone match the Windows ones on a non-US machine,
  including AltGr and dead keys.
- [ ] Close the VM window at a moved, non-maximized size; the next launch opens
  it there. Unplug the monitor it was on; the next launch opens maximized.
- [ ] Reclaim disk space from the tray after deleting a large file in Omarchy;
  after shutdown the disk file is smaller and Windows free space never fell
  below 4 GiB during the pass. Repeat with Settings showing the new size.
- [ ] Settings: Rendering set to CPU and back to Automatic takes effect on the
  next launch; Guest CPUs shows the automatic choice for this PC.
- [ ] Remove Try Omarchy from Apps & features on a copied install; the folder,
  its shortcuts, and its entry are gone and the original install still runs.

## Hardware matrix facts to record

The nested test VM cannot answer these; every physical report should.

- GPU vendor, driver version, and whether the desktop came up on GPU or CPU
  rendering (`render-probe.json` in the data folder records the result).
- Idle CPU of `qemu-system-x86_64w.exe` in Task Manager five minutes after the
  desktop settles, with and without a video playing.
- Whether the Windows microphone indicator lights at launch before any app
  records, and whether it lights when one does.
- Display scaling in use, and whether text in Omarchy is crisp at 125 percent
  and above; whether moving the window between monitors with different
  scaling leaves it usable.
- Whether audio follows a headphone plug-in mid-session.
- Whether the guest browses with the machine's VPN connected.

## Data and update recovery

Create a few identifiable files before each test and compare them afterwards.
Use the copied guest for interruption and low-space tests.

- [ ] A copied pre-transfer installation updates through its original signed
  feed into the Omacom candidate. Record starting and target versions, release
  URLs and redirects, signature verification, and preserved files. Force a
  rollback and confirm the copied installation still boots. Local candidate
  tests do not replace checking the public URLs after publication.
- [ ] An existing guest upgrades without replacing its OS, account, or files.
- [ ] Interrupt the first updated boot before readiness, then relaunch. Previous
  launcher and payloads return and the existing files remain readable.
- [ ] A preview that misses the bridge installs the bridge first, then stable
  on its next update check. Test direct stable install and stable-to-stable too.
- [ ] Grow the disk, verify capacity inside Omarchy with `df -h /`, and check the
  files. Lowering the preference and rolling back the launcher never shrink it.
- [ ] From Settings, back up a stopped VM, cancel an operation, and restore a
  separate copy. Verify its Start Omarchy and Settings shortcuts point to that
  copy, then boot it and compare the guest files.
- [ ] Reset from Settings after taking a backup. Confirm first-run setup on the
  next launch and that the previous disk is retained under `vm/before-reset-*`.
  Check keyboard navigation, file pickers, progress, and scaling of the controls.
- [ ] Low host free space produces a useful error and leaves the existing guest
  usable after space is freed.
- [ ] Configuration export restores the selected theme, personal configuration,
  and added packages on a separate fresh Omarchy installation.

- [ ] In portable mode, reset with `-portable -fresh`, reach a new guest, and
  verify that `vm/before-reset-*` retains the previous QCOW2 disk and identity.
  With Omarchy closed and the matching original factory payload restored,
  returning that retained pair to `vm` should recover the previous guest.

## Coverage

Track results for Intel and AMD CPUs, integrated and discrete Intel/AMD/NVIDIA
GPUs, Windows Home and Pro, and each advertised Windows version. Include a
physical system with full Hyper-V enabled and a lower-memory host. A nested VM
helps with automation but cannot replace physical GPU and hypervisor coverage.

The runtime-specific checks remain in [RUNTIME-VALIDATION.md](RUNTIME-VALIDATION.md).
