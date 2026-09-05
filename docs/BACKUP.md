# VM backup and restore

The development build supports backup, restore, and reset controls in Settings
for stopped standard installs. Portable installs are not supported yet. These
operations are separate from
[configuration export](MIGRATION.md), which transfers your setup to a physical
Omarchy installation.

## Use Settings

Open Settings and use **Back up**, **Restore**, or **Reset guest** under Backup
and recovery. Close the Omarchy VM first. Recovery uses the settings already
saved on disk; edits in the Settings window remain available after the operation.

- **Back up** opens the Windows save dialog. Choose a new ZIP filename outside
  the installation folder.
- **Restore** asks for a backup and a parent folder, then creates a separate
  restored installation. It adds **Start Omarchy** and **Settings** shortcuts
  inside that new folder. Existing Windows shortcuts are unchanged.
- **Reset guest** offers a full backup first, then asks for confirmation. A
  failed or cancelled backup stops the reset. The factory disk is prepared
  before the old disk is moved into a `vm/before-reset-*` folder. The next normal
  launch starts first-run setup. Windows shared folders and launcher settings
  are kept.

Backup and restore show file progress and support cancellation. Reset retains
only the previous writable disk, not a complete backup of its boot files and
runtime. Keep a full backup before updating or removing the old installation.
The retained disk continues using host space until you remove it yourself.

## Create a backup from PowerShell

Shut down Omarchy and close the launcher first. From PowerShell:

```powershell
.\TryOmarchy.exe -backup "D:\Backups\omarchy.zip"
```

The destination folder must already exist on an NTFS or ReFS drive, outside the
Try Omarchy data folder. Choose a new filename; an existing backup is never
overwritten. Add `-dir "D:\TryOmarchy"` if you normally use an explicit data path.

The ZIP includes the writable disk, factory image, matching kernel and initramfs,
bundled runtime when present, and launcher settings. Shared Windows folders,
external QEMU installations, logs, and the launcher executable are not included.
Keep a copy of the launcher used to create the backup.

A backup contains personal guest files, including any credentials stored in the
guest. It is not encrypted. Store it privately and restore only backups you trust.

Backup refuses a locked disk or a pending update. Finish the update and shut down
normally before trying again. Each archived file has a SHA-256 checksum that is
verified during restore. The ZIP appears at the chosen filename only when the
backup finishes.

## Restore to a separate folder

Close Try Omarchy, then choose a folder that does not exist. Its parent must
already exist on an NTFS or ReFS drive:

```powershell
.\TryOmarchy.exe -restore "D:\Backups\omarchy.zip" -dir "D:\OmarchyRestored"
```

Restore checks and extracts files into a temporary folder before publishing the
new data folder. An error or cancellation leaves the existing installation and
backup unchanged. It never replaces an existing data folder.

Start the restored copy explicitly:

```powershell
.\TryOmarchy.exe -dir "D:\OmarchyRestored"
```

This does not change the default installation location or existing shortcuts.
Settings may reference shared folders or SSH public keys on the original PC;
review them before starting a restored copy on another machine.

## Space and validation

Both operations conservatively require free space for the full logical size of
all included files, plus 1 GiB. Compression and sparse files can reduce actual
usage, but are not counted on for the space check. Large disks can take a while
to read even when mostly empty.

Automated tests cover copying, checksums, cancellation, disk locking, low space,
and preservation of existing folders. A restored guest still needs a Windows
boot test before release. Keep the original installation until you have checked
the restored copy. The Settings dialogs and a restored guest boot still need hands-on Windows
validation before release (#36).
