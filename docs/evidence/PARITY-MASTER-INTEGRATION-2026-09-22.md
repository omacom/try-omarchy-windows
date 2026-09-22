# Parity and master integration: September 22, 2026

This is unpublished engineering validation for `codex/parity-master-integration`.
It does not publish a runtime or release and does not replace the September 21
pinch, audio, fresh-image or nested-Linux evidence.

## Integrated source

Merge commit `dca2901` combines the tested parity checkpoint `67f27b4` with
`origin/master` at `3c0e532`. It preserves the pre-boot launcher, startup audio
choices, opt-in pinch bridge and nested-Linux work alongside master's host-aware
resource profiles and GPU application tooling. Commit `aacf340` removes the stale
public-roadmap reference from the parity tracker.

The integration also stores selected Windows Core Audio endpoint IDs separately
from the SDL-friendly names. At startup, a stable ID resolves the current name;
missing or ambiguous endpoints retain the existing name-based fallback. This is
still startup-only routing. It does not add live stream switching.

## Validation

- Linux `go test -race ./...` and `go vet ./...` passed.
- Windows `go vet -unsafeptr=false ./...`, cross-build and test compilation passed.
- The guest contract passed separately during the merge review.
- The physical Windows laptop was online, interactively signed in and idle. The
  focused native endpoint enumeration and Settings persistence tests passed
  against the retained r17 runtime.
- The complete native Windows suite passed with **378 top-level passes, 32 skips
  and zero failures** from a fresh snapshot of the combined worktree.
- The active guest installation was not started or changed. No candidate launcher
  or QEMU process remained after testing.

Final unsigned artifacts retained under `D:\TryOmarchy-Parity-20260921`:

| Artifact | SHA-256 |
| --- | --- |
| `TryOmarchy-parity-master5.exe` | `3c9c4818ac80de0770650a3610383ab21f2d7626300cd6585f3593263f1363a7` |
| `TryOmarchy-parity-master5-tests.exe` | `fef1403c2947b1a1963fac2f40103cd91c5e396e154aef88efcc7a2e062975d7` |
| `parity-master5-source.tar.gz` | `1a4e0b687be1271c41167ca65b12642f135c192f4f226eda986b872ad2a0fb14` |

The first diagnostic candidates exposed and then corrected a COM call-signature
error that returned endpoint IDs without friendly names. Fresh filenames were
used throughout. Their on-demand test tasks have no automatic triggers.

## Remaining boundaries

Only the laptop's built-in speaker and microphone were present. Stable-ID
enumeration, persistence and name resolution passed, but a physical endpoint
rename, unplug/replug and switching between two devices remain untested. Live
guest-driven audio routing, Windows Hello sudo, bridged networking, physical
finger pinch and the broader final-candidate/release gates remain open. Pinch is
still opt-in, and no release was published.
