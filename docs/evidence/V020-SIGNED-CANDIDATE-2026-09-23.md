# v0.2.0 signed candidate on the Windows laptop (September 23, 2026)

The public release was still `v0.1.0` during this test. GitHub Actions
prepared draft `v0.2.0` in [run 35861517038](https://github.com/omacom/try-omarchy-windows/actions/runs/35861517038),
including a built guest and headless boot smoke. The draft manifest's SHA-256
was `597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`.
All eleven downloaded draft assets matched that manifest. The compressed guest
rootfs was `a4d734f3d95f537fb5fb0fa06fd351b74cb9358726daaf51dd4dad65bd53ab4d`;
its decompressed SHA-256 was
`02ff8a0e28ae4dc69bc54a4193adc73f6e089f11c6549346da3d78737397d119`.
The release source pin was commit `1ae1a83a2185fdc5e63d5f08ade42d4508e2488e`.

[Signing check 35862627687](https://github.com/omacom/try-omarchy-windows/actions/runs/35862627687)
produced the signed test launcher. Its SHA-256 was
`f3c35ab82513a4f2d8dc6a7a4f946e6ae072784fcda95d0c5049ceaf34babd00`.
Windows reported Authenticode `Valid`, FileVersion `v0.2.0`, and ProductVersion
`v0.2.0` for this exact file.

## Physical upgrade and boot

The AMD Radeon Windows 11 laptop ran the candidate from an isolated copy of a
previously accepted clean `v0.1.0` installation. The source guest and the
ordinary installation were untouched. The signed launcher fetched the draft
payload over a local SSH reverse tunnel with the draft manifest hash pinned and
used its downloaded bundled r19 runtime. The candidate runtime QEMU executable
matched the tested r19 runtime QEMU hash
`8c5510b76713adb9edf9a1659e2ee1df34fd00cbfd05105912f53291126c7c75`.
The QEMU command had `free-page-reporting=on` and the VirGL GPU device.

The guest reached userspace and the launcher logged `guest update v0.2.0
confirmed after userspace reported ready`. The guest had kernel
`7.2.6-arch2-1`, systemd state `running`, no failed units, charging battery at
99%, active PipeWire and WirePlumber, a sink and source, and `virtio_balloon`
loaded. The retained user file `~/Documents/v010-clean-marker.txt` still had
SHA-256 `d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`.
Guest SSH used an existing pinned host key from the copied source guest; it was
not accepted without fingerprint verification.

## New product paths

- `try-omarchy-host-settings` opened native Settings from the guest. Its new
  fullscreen display and approved app controls were present. With the VM
  restored, the Settings window ranked above the VM in Windows z-order.
- An approved Notepad ID appeared as a guest launcher entry. Invoking it from
  the guest launched Windows Notepad and minimized the VM. Removing its host
  approval rejected another guest launch and removed the launcher entry on
  sync. The test Notepad process was stopped afterward.
- The guest powered off cleanly and the launcher exited. The installed guest
  receipt contained the draft manifest and decompressed rootfs hashes. A second
  launch booted the same GPU guest without downloading or replacing the rootfs;
  the retained user file and revoked app state remained intact.

The laptop had one active physical monitor, so target-monitor placement on a
second screen was covered by Windows native display enumeration and local tests,
not physical dual-monitor acceptance. The host had no enrolled Windows Hello
device, second playback endpoint, or TAP bridge, so those separate feature
issues remain open. The Intel/NVIDIA desktop was kept in Omarchy at the owner's
request. These limits do not imply a broad hardware release gate.

## Interrupted-update recovery

A second disposable copy of the clean `v0.1.0` guest was made after the normal
upgrade test. The signed `v0.2.0` candidate downloaded and expanded the draft
payload into this copy. A watcher stopped both the launcher and QEMU at
08:31:01 local time, about four seconds after the GPU boot began and before
`guest userspace announced ready` appeared. The pending guest and runtime
retained their `guest.previous` and `runtime.previous` directories.

On restart, the launcher logged `using restored guest and runtime for this
recovery launch` and booted without downloading the failed payload again. The
guest install receipt had its original manifest SHA-256
`3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`.
Systemd reached `running`; the preserved file still matched
`d7c3d98dd030ccf076e9bc8d1529ef6814c314704c610486d7256e93744e7ade`.
The recovered guest then received a clean poweroff. These observations prove
recovery for an interruption before userspace readiness on this copied
installation. They do not simulate every possible power-loss timing.

## Public release verification

[Publish workflow 35867779077](https://github.com/omacom/try-omarchy-windows/actions/runs/35867779077)
passed signing and public tagged/Latest asset checks. The public
[`v0.2.0` release](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.2.0)
is neither draft nor prerelease and is the repository's `Latest` release. Its
freshly signed launcher SHA-256 is
`57f3b920f8384fb8eddd6715d4600b9ec11eb22a5323d1c6b377ad33cb7eaa8d`;
the public `SHA256SUMS` remains
`597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`.
The public `update-v2.json` points to `v0.2.0` with those launcher and payload
hashes. A separate copied installation running the signed public `v0.1.0`
launcher authenticated that feed and installed the public `v0.2.0` executable;
Windows reported its exact public SHA-256 and Authenticode `Valid`.

The public payload update then booted the GPU guest, logged
`launcher update v0.2.0 confirmed after healthy boot` and `guest update
v0.2.0 confirmed after userspace reported ready`, and wrote the public
`v0.2.0` URL and draft-identical manifest hash to its guest receipt. Systemd
was `running`, the laptop battery was 99% `Charging`, and the preserved user
file still matched its original SHA-256. That first boot selected the laptop's
separately installed `C:\WINQ-EMU`, which Try Omarchy deliberately leaves
user-managed. A controlled second launch forced the bundled runtime, downloaded
public r19, and booted the GPU guest again with
`-device virtio-balloon-pci,free-page-reporting=on`. The bundled QEMU SHA-256
was `8c5510b76713adb9edf9a1659e2ee1df34fd00cbfd05105912f53291126c7c75`,
matching the earlier physical r19 test. Systemd again reached `running`,
`virtio_balloon` was loaded, battery remained 99% `Charging`, and the same
user file hash was preserved. The launcher then logged `runtime update v0.2.0
confirmed after guest userspace reported ready`. A separately managed external
runtime may lack the r19 free-page feature; the ordinary bundled path is the
validated one. The guest powered off cleanly. Both disposable test copies and
their on-demand Windows tasks were removed; the original clean installation
remained present. Small candidate logs and checksums remain in the laptop's
`D:\TryOmarchy-v0.2.0-20260923` evidence folder.
