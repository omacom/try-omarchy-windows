# Offline portable mode

Portable mode runs the Windows launcher and a persistent Omarchy guest from
removable storage without downloading anything on the target PC. It is an
experimental launcher mode; preparation and redistribution tooling are outside
this core change.

## Host requirements

- 64-bit Windows 10 or 11
- Intel VT-x or AMD-V/SVM enabled in firmware
- Windows Hypervisor Platform (the launcher can enable it with UAC and one
  restart)
- 8 GB RAM minimum; 16 GB recommended
- exFAT or NTFS removable storage; USB 3 or faster recommended

FAT32 is unsuitable because the decompressed factory image exceeds its 4 GB
single-file limit. A 32 GB or 64 GB drive leaves useful room for persistent
guest changes.

## Layout

The executable is started with `-portable` and expects this sibling layout:

```text
TryOmarchy-Portable/
|-- TryOmarchy.exe
|-- payload/
|   |-- SHA256SUMS
|   |-- winq-emu-alpha10-portable.zip
|   |-- build-spec.json
|   |-- vmlinuz-linux
|   |-- initramfs-linux.img
|   `-- rootfs.ext4.zst
`-- data/                         created on first run
    |-- runtime/                  extracted WINQ-EMU
    |-- guest/rootfs.ext4         verified factory guest
    |-- vm/disk.qcow2             persistent changes
    `-- vm/disk.qcow2.backing-sha256
```

The payload files must come from the release identified by `defaultReleaseURL`.
The local `SHA256SUMS` is authenticated against the independent digest embedded
in the launcher; carrying a modified manifest beside modified payload files does
not bypass verification. The compressed archive and decompressed rootfs are
checked against their separate manifest entries.

Small guest artifacts are copied through `.part` files, the rootfs and QCOW2
disk are published atomically, and cancellation removes staging files without
damaging a complete prior installation. Install receipts preserve the verified
fast path on later launches.

The QCOW2 backing path is relative, so Windows may assign a different drive
letter on another PC. The Windows Hypervisor Platform restart marker stays in
the host's local app-data directory rather than travelling with the USB.
The digest beside the QCOW2 disk binds its persistent changes to the exact
factory image. Replacing a portable payload without resetting its data is
refused instead of silently running an overlay against the wrong backing
image. Keep the old payload with its data, or use `-portable -fresh` to adopt a
new payload.

## Running and reset

From Command Prompt in the portable folder:

```bat
TryOmarchy.exe -portable
```

Shut Omarchy down from its system menu and wait for the QEMU window to close
before ejecting the drive. To reset only the writable guest state, start with
`-portable -fresh`; the authenticated factory image and payload remain intact.

Development builds prepare the replacement before moving the previous disk and
its `.backing-sha256` file into `vm/before-reset-*`. A failed reset rolls those
files back when possible. If Windows prevents rollback, the error identifies
the retained folder. Keep both files and the matching original factory payload
for recovery. A retained QCOW2 disk must be returned to the `vm` folder before
use because its factory path is relative to that folder.
