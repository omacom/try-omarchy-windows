# Move an installation

Available since v0.0.18-preview.

Close Omarchy, save any Settings changes, then open Settings and choose **Move…**.
Choose a local drive or parent folder. Try Omarchy creates a `TryOmarchy` folder
there, checks space, and copies and verifies your installation before switching
locations. An occupied destination is never replaced.

Your guest files, installed apps, settings, bundled runtime, recovery folders,
and other supported files inside the installation folder move together. Sparse
disks keep their capacity and empty regions. Shared Windows folders and external
runtime paths stay where they are. Standard installs need a local NTFS or ReFS
drive. Portable installations, linked files or directories, and files with
additional Windows data streams are not supported by this operation.

The launcher updates its remembered location, owned Start-menu and Desktop
shortcuts, and Apps & features entry. Explicit `-dir` launches from this version
also follow the moved location. Use the current launcher after moving; older
downloaded executables do not understand move history.

Start the moved installation and check your files. After a successful guest boot,
**Remove previous…** becomes available in Settings. Close Omarchy before using
it. This verifies the original again and removes only inventoried files. Changed
files, additional streams, or unexpected content stop cleanup. The original
continues to use disk space until cleanup completes. Finish this cleanup before
moving another installation or uninstalling the moved copy.

## Interrupted operations

Copying can be cancelled. The original remains intact. Opening Try Omarchy after
an interrupted copy removes its staging files and continues with the original.

Once copying and verification finish, Try Omarchy completes the location switch
before accepting another operation. If this is interrupted, open the current
launcher again to finish the switch. Reconnect any unavailable drive first.
The launcher does not fall back to the retained original after activation,
because that could discard files written in the moved guest.

If cleanup is interrupted, choose **Remove previous…** again. Unexpected files
are kept and reported instead of being recursively deleted. Keep the retained
copy until you have checked the moved guest, even if copying completed without
errors.

Move history is stored separately in `%LOCALAPPDATA%\TryOmarchy-host`. Do not
delete or edit it while a move or retained-copy cleanup is outstanding.
