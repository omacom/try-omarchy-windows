# Contributing

For a bug, describe what happened, how to reproduce it, and your Windows version, CPU, GPU, and Try Omarchy version. The [bug form](https://github.com/omacom/try-omarchy-windows/issues/new?template=bug_report.yml) and [testing guide](docs/TESTING.md) list useful details. Review diagnostics before uploading them; do not attach guest disks or backups.

For larger features, check the [open issues](https://github.com/omacom/try-omarchy-windows/issues) and [Mac parity tracker](docs/MAC-PARITY.md) first. Keep changes focused and explain what users will notice.

## Repository layout

- `app/`: Go launcher, Windows integration, and unit tests
- `guest-build/`: pinned Linux guest source and patch series
- `runtime-build/`: QEMU and graphics runtime build and patch series
- `scripts/release/`: artifact validation, signing helpers, and guest build checks
- `docs/`: user guides, acceptance instructions, and evidence

## Build and test

Use the Go version declared in `app/go.mod`. From `app/`:

```sh
go test -race ./...
go vet ./...
GOOS=windows GOARCH=amd64 go vet -unsafeptr=false ./...
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o ../TryOmarchy.exe .
```

The race check requires a C compiler. Cross-compiling checks the Windows build; it does not run Windows tests. On Windows, run `go test ./...` from `app/` and verify the native behavior affected by your change.

For release helper or runtime recipe changes, run these from the repository root:

```sh
python3 -m unittest discover -s scripts/release -p 'test_*.py'
python3 -m unittest discover -s scripts/gpu -p 'test_*.py'
python3 runtime-build/validate-lock.py
```

For guest changes, follow [guest-build/README.md](guest-build/README.md) and run `scripts/release/build-guest.sh --contract-only`. This fetches and patches the pinned guest source; a contract test is not a full VM boot.

## Validate the affected behavior

Add a regression test for a bug when it can reproduce the failure. Use a separate data folder and copied guest for tests that change installations, updates, or recovery state. Keep the original guest and backup untouched.

The [nested Windows VM harness](scripts/vmtest/README.md) is useful for launcher flows. GPU, audio devices, touchpads, Windows Hello, and monitor behavior need relevant physical hardware. Report what you tested and what remains untested. See [TESTING.md](docs/TESTING.md) for the detailed checklist.

## Pull requests

Describe the problem, the resulting behavior, and the checks you ran. Include screenshots for visible changes and exact versions or hashes for hardware acceptance. Keep unfinished work in a draft PR.

Merging code and publishing a release are separate steps. Follow [RELEASING.md](docs/RELEASING.md) for signed candidates and release acceptance.
