# Signed v1 candidate clean install, September 22, 2026

This is acceptance evidence for the unpublished `v1.0.0` draft, not a release
announcement. The public download remains `v0.0.20-preview`. Testing used a
new, isolated installation on the AMD Ryzen 5 5625U Windows 11 Business laptop
(build 26200; Radeon driver `31.0.21921.1000`). Existing guest disks were
left intact. The laptop's D: drive is slow external storage, so elapsed setup
time does not represent installation time on an internal SSD.

The exact signed launcher SHA-256 was
`d1753ae2121ad7e8d30558e5a9d0719db506d48bfc9293edced512fbe65168dc`.
Windows reported a valid Authenticode signature and `v1.0.0` file and product
versions. The draft payload was fetched through a loopback endpoint with an
explicit `SHA256SUMS` SHA-256 pin of
`f388d0c041aa8b4a160fa743c6af1bcacf5c1b5d10a691c3f226b15727024e58`.
The unpacked factory image matched the authenticated manifest digest
`d717802599dac97b8b6e7be0501bea80e61ec3cddb9e65a3ff85aac23e75a321`.
The bundled QEMU executable SHA-256 was
`564d900a7839c100c716fd77aa0e40c3a24524eb540ea43347832562a966718c`.
The launcher bypassed the laptop's older external QEMU for this test.

The first-run progress window downloaded and verified the runtime and guest,
unpacked the 7 GiB image, prepared a sparse 24 GiB writable disk, and showed
the Start Menu/Desktop shortcut choice. Continuing with the default Start Menu
choice booted WHPX with VirGL/Venus GPU acceleration. The new guest reached
Hyprland and systemd `running` with no failed units. The live desktop was
inspected in a Windows screenshot; the guest had active PipeWire and
WirePlumber, a playback sink and capture source, and the Windows share mounted
read/write at `/mnt/host`. The guest ran kernel `7.2.6-arch2-1` and saw a
24 GiB root disk. A new `~/Documents/v1-clean-marker.txt` had SHA-256
`3423718ef602456bde8d374d9da0ec4f974d8c1112f5f740ceeb84a91965e2d9`.
`sudo -n systemctl poweroff` shut down the guest cleanly; QEMU and the launcher
exited.

The first-run shortcut choice exposed a multi-install bug: choosing Start Menu
replaced links owned by the laptop's existing default installation. The original
desktop link still identified that installation. Both Start Menu links were
restored to its stable launcher and verified. Source PR
[#154](https://github.com/omacom/try-omarchy-windows/pull/154) now checks all
selected shortcut paths before writing any, preserves foreign links, and
passes Windows-native regression tests. That fix is newer than this signed
candidate, so a future signed build must include it.

## Backup and restore

The signed candidate completed a full backup from the stopped clean install.
The archive was 6,620,280,172 bytes. The same signed candidate restored it to
a separate folder, validating the archive's per-file checksums. The original
clean installation and backup remained present. That candidate's command-line
restore did not make folder-local launchers or record that global shortcuts
were already offered. To avoid overwriting the laptop's existing Start Menu
links, the test recorded the shortcut-offer marker in the restored folder
before boot. Source PR
[#156](https://github.com/omacom/try-omarchy-windows/pull/156) now applies
the Settings restore flow's folder-local launcher finalization to command-line
restore; this source fix needs a new signed build.

The restored copy booted with GPU acceleration, reached the visible Hyprland
desktop, and reported systemd `running` with no failed units. Its saved file
still had SHA-256
`3423718ef602456bde8d374d9da0ec4f974d8c1112f5f740ceeb84a91965e2d9`.
PipeWire and WirePlumber were active and `/mnt/host` was mounted. After clean
poweroff, an explicit `-render cpu` launch reached the visible desktop with
the same marker and no failed units. The QEMU command line used WHPX,
`virtio-gpu-pci`, and SDL `gl=off`; the guest command line specified
`tryomarchy.render=cpu`.

For automatic fallback, only the next launcher process received
`SDL_OPENGL_LIBRARY` pointing to a deliberately nonexistent DLL. Its GPU
attempt exited at startup with `0xc0000005`; the signed launcher logged the
failure and retried with CPU rendering on attempt 2. That guest reached the
visible desktop, retained the marker hash, and had no failed system units.
The environment override ended with the test process. This is controlled
failure-recovery evidence, not an observed graphics-driver failure.

After clean guest shutdown, the signed app's `-uninstall` flow removed only
the restored test folder. The UI showed that exact path; the duplicate backup
was skipped because the clean source already had a full backup. The folder
was absent afterward, while the clean installation and backup remained.
The laptop's original Start Menu and Settings links still targeted its default
installation. The successful removal dialog was inspected before dismissal.

## Boundaries

The signed candidate was built from source commit
`f23ef3d2dd13502a2b58c0770f87aa48287e2684`; later About, shortcut,
and command-line restore source changes are outside its binary. A new exact
signed build must include those changes before a normal release. The
[signed upgrade and rollback record](V1-SIGNED-CANDIDATE-2026-09-22.md)
covers the existing-install path. Physical pinch results are in the
[r18 acceptance record](PINCH-R18-PHYSICAL-2026-09-22.md). This clean-install
test does not claim a final 1.0 publication decision or broader hardware
coverage.
