# File drops into apps on the laptop, September 27, 2026

The AMD Windows 11 laptop ran a launcher built from the drop-to-app branch
(SHA-256 `a0882bdaad012a8fd4543b1dd7619cd9dcd5edd41b14a28376fb15b9854d9797`)
with a guest image built from the same patches (through `0100`, compatibility
revision 38). It upgraded the disposable v0.5.0 `fresh` test install; the
guest then had `/usr/local/lib/try-omarchy/drop-drag`.

Drops were native `WM_DROPFILES` messages posted to the QEMU window from an
interactive task, with the Windows pointer placed at the drop point. The guest
had two Chromium app windows on a local test page whose drop handler reports
the dropped file's name and length, plus a Files window on `~/Documents`.

| Drop | Launcher log | Result |
| --- | --- | --- |
| `drop-sample.txt` (4321 bytes) onto the right Chromium window, pointer left in place | `file drop: dragging into the app under the pointer` | Page reported `dropped drop-sample.txt:4321`; the file was also in Downloads |
| `drop-moved.txt` onto the left Chromium window, pointer moved away 0.3 s after the drop | `not dragging into the app: the pointer moved after the drop` | Page unchanged; the file stayed in Downloads |
| `drop-folder.txt` onto the Files window | no drag | Copied straight into `~/Documents`, nothing in Downloads |

The test files were removed and the guest powered off afterwards. A physical
drag from File Explorer was not part of this run.
