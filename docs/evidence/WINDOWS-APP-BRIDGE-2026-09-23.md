# Approved Windows app bridge candidate — September 23, 2026

This is physical acceptance for the **unreleased** phase 1 of [issue #160](https://github.com/omacom/try-omarchy-windows/issues/160). The test host was the AMD Ryzen 5 5625U Windows 11 Business laptop, build 26200. Tests used the isolated `D:\TryOmarchy-Candidate-20260919\fresh29-signed` installation and r19 runtime. The user's normal Try Omarchy installation was not changed. The Intel/NVIDIA PC remained in Omarchy.

## Artifacts

- Fullscreen source launcher with tray restore fix, SHA-256 `8bc17f7842117125d61d4052f6ccad89b81768aecec1cad11cd739950d36a29e`. The later UI control-ID build used for the native Apps-page test was `a0b16c1d719db28188d76093760bf01cd2480c0a5e954dafe0b579779c0aedd4`; it did not change app launching or tray restoration.
- New guest `rootfs.ext4` SHA-256 `27cfc55ff72e468c3aafe1a489a2ed01c20f26b299d240d623109bf1d11181a9`; `initramfs-linux.img` SHA-256 `7fcff4831b9342db098e98b4599a73f6a62bb992a5f8f4d3cf80e818995314c0`. The factory image declares compatibility revision `32` with kernel `7.2.6-arch2-1` and contains the executable guest request and sync helpers. The full image build and headless instant-trial boot smoke passed.
- The build package lock changed only `gpu-screen-recorder` from `6.1.2-1` to `6.1.3-1` after the Arch repository moved. The exact transaction was reviewed before the successful image build.

## Laptop checks

1. The new native **Apps** page appeared in the Settings window. A separate interactive native test loaded an approved app, selected the Apps page, removed the entry, saved and reopened the approval store; it was empty. Approval validation, storage/revocation and bounded one-shot agent tests passed in a Windows test executable.
2. An isolated host allowlist entry for `C:\Windows\System32\notepad.exe` was sent to the guest as **ID and display name only**. The guest agent version 4 created `Windows: Notepad` in `~/.local/share/applications`. The executable path did not cross into the guest. This physical pass installed the newly built guest helper scripts into the older isolated test disk, because the new factory image had not yet been copied to the laptop.
3. Running the guest helper started Notepad on the Windows desktop and minimized the fullscreen Omarchy window. The Windows process ran in the interactive user session. The user's normal Windows environment remained the host; no app window was embedded in Hyprland.
4. Removing the host approval immediately made a second guest launch return an error. The guest `.desktop` entry disappeared on the next agent synchronization. The existing Notepad process remained open until explicit test cleanup, so revocation did not terminate a running Windows app.
5. The original tray **Open Omarchy** action did not restore a minimized VM. The source fix now uses `SW_RESTORE`; a second fullscreen run with that fix returned to the Omarchy desktop through the tray. The guest display had idled during unattended testing and woke after cursor movement. Its normal lock/idle behavior was preserved.

The guest shut down cleanly after both runs. The test launcher, approval file, temporary scheduled tasks and launched Notepad processes were removed from the laptop; the isolated D: candidate artifacts were retained for future signed-candidate comparison. The public `v0.1.0` release is unchanged. A signed application candidate carrying this guest image remains necessary before describing the bridge as shipped.
