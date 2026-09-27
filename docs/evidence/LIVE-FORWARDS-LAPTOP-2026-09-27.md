# Live port forwarding on the laptop, September 27, 2026

The AMD Windows 11 laptop ran a launcher built from the live-forwards branch
(SHA-256 `6427e7469790e85361842d713ee55c99d5f46f12c1d4a7130e52285f4835db67`)
against the disposable v0.5.0 `fresh` test install. Its saved settings carried
`tcp:2256:22` and an SSH key, and the launcher ran without `-forward` or `-ssh`,
so the saved list applied. A small web server listened on guest port 8000.
Settings were edited while Omarchy ran, and each check fetched
`http://127.0.0.1:18090/` from Windows.

| Change | Result | Launcher log |
| --- | --- | --- |
| Add `tcp:18090:8000` | Answered with the guest page within about 4 seconds | `applied 1 added, 0 removed while running` |
| Remove it | No answer | `applied 0 added, 1 removed while running` |
| Add it again plus a LAN forward on the laptop's address | Local forward answered; LAN forward waited | `tcp:192.168.1.71:18091:8000 changes at the next launch` |
| In-guest reboot | The live-added forward was applied again after QEMU restarted | `applied 1 added` after the relaunch |

The first attempt surfaced a separate problem: PowerShell had saved
`settings.json` with a UTF-8 byte-order mark, and the launcher refused to read
it. The launcher now accepts that mark. The install's original settings were
restored and the guest powered off afterwards.
