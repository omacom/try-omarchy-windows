# 7Wdev fork

Source: https://github.com/7Wdev/try-omarchy-windows

This fork tracks `omacom/try-omarchy-windows` and develops a Windows-native
NVIDIA backend for QEMU/WHPX. Two fresh guests now pass a
[UWSM-managed 4K GPU desktop and five application lifecycles](https://github.com/7Wdev/try-omarchy-windows/blob/2abc463db5cdb8c2b62b3c0526fe60932f9acde7/docs/LIVE-UWSM-NATIVE-INPUT-DISCOVERY.md)
with active service-state proof, demand-allocated staging and complete terminal
actor reclamation below the unchanged usage guard, plus guest keyboard/tablet
discovery. Windows-window input remains unverified. Earlier failed runs remain
retained; long-run stability is open. The released launcher continues using the
upstream graphics runtime; supported device integration, normal launcher/input,
sustained use and near-native performance remain unfinished.

The current backend design is [virtio-dxg](VIRTIO-DXG.md): Microsoft's
`dxgkrnl` over virtio in the guest and a D3DKMT executor in QEMU, replacing
the experiment's userspace interposer while reusing its hardware findings.

## Upstream updates

`Sync fork with upstream` runs daily at 06:17 UTC and can be dispatched manually.
It merges upstream `master`, validates on Windows and Linux, then publishes
only a passing fast-forward update to this fork's `master`. Fork commits are
preserved. It never rebases, resets or force-pushes. Conflicts stop the run with
the fork head retained. Failed validation leaves a `sync/upstream-*` candidate
branch for inspection. Concurrent edits of `master` stop publication safely.

GitHub Actions must be enabled. GitHub can suspend schedules after repository
inactivity, so check the Actions page when returning after a long break.
Graphics changes still require physical acceptance even when CI passes.

GitHub's built-in Actions token cannot publish upstream changes to workflow
files. Such updates stop the sync run before publication, preserving `master`.
Apply those merges with authenticated Git credentials permitted to update
workflows, then run the same validation before pushing. The 2026-10-09 run
stopped for this reason; all 63 missing commits were merged manually on
2026-10-10 without resetting fork or GPU development history.

For a local merge (the script does not push):

```powershell
git remote add upstream https://github.com/omacom/try-omarchy-windows.git
git fetch upstream master
python scripts/fork/prepare_sync.py
```

If `upstream` exists, use `git remote set-url upstream ...`. The script requires
a clean checkout and aborts a conflicted merge. Follow CONTRIBUTING.md checks
before publishing a local merge.

## Build your version

With the Go version in `app/go.mod`:

```powershell
scripts/fork/build.ps1
```

`dist/TryOmarchy-7Wdev.exe` embeds fork identity, defaults to the
`TryOmarchy-7Wdev` data directory and blocks official launcher replacement.
Rebuild after source merges: executable updates and source updates are separate.
Initial guest/runtime downloads still use pinned, verified upstream payloads.
A fork release feed and your own signing keys have not been set up. The builds
are unsigned development artifacts.

The generic upstream `go build` without fork linker flags retains official
update behavior. Use the fork build script for your version. The GPU experiment
builds separately; see its [README](https://github.com/7Wdev/try-omarchy-windows/blob/2abc463db5cdb8c2b62b3c0526fe60932f9acde7/experimental/windows-native-gpu/README.md).
