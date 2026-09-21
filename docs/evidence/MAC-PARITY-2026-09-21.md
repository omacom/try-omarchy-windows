# Launcher parity and nested KVM validation — September 21, 2026

This records local source and unsigned Windows candidates. It is not a public
release, signing approval, or a declaration of complete Mac feature parity.
See [the comparison tracker](../MAC-PARITY.md) for remaining implementation work.

## Candidate identity

Final launcher candidate: `TryOmarchy-parity6.exe`.

- Launcher SHA-256:
  `ce60e7c26415ef7edfb3849cbf0a75b7d280ec479c136a35162ebd8057bd03e6`.
- Native test executable SHA-256:
  `7e49c4aba10be13f9c58de308bff528d2ee668450eba8d2b5576d9c133f2585f`.
- Windows host: Ryzen 5 5625U / Radeon graphics, Windows 11 Business build 26200.
- Guest: public v0.0.20-preview payload, kernel `7.2.6-arch2-1`, compatibility 29.
- Guest/runtime manifest SHA-256:
  `bbdf1d478dc0a15fd105cde47057ab85190510b59742ffabf8da1a94117d2d49`.
- Installed runtime archive SHA-256:
  `8b0e198356dd4362478f91f6cebf0e71e7b59558829233f6ebd9b35e9b4debdc`.

Checksums were compared on Linux and Windows before running each candidate.
The guest image and QEMU runtime were not rebuilt or replaced in this work.

## Implemented and checked

- Ordinary desktop launches open native General, Devices, Advanced and Recovery
  controls before boot. **Launch Omarchy** saves and starts the supervised guest.
  Explicit runtime commands retain direct startup; `-start` skips the menu.
- A Windows named object prevents competing launcher windows from racing the
  first-run location choice without taking the VM lifecycle port. A moved
  launcher releases that object before handing off to its replacement.
- About and first-run location selection use labelled native buttons. About
  preserves the credits, project link and separate launcher-update action.
- Devices links to Windows sound settings. This is a default-device control,
  not live per-app audio routing or the Mac guest-side device chooser.
- Existing copyright attribution is retained in LICENSE alongside contributor
  credit. Omacom resource branding and third-party notices are retained.
- Native tests cover repeated choice-window creation, button actions, Escape,
  hidden process startup, first-run default/cancel, launcher locking, Enter on
  Close, saving settings with Enter without booting, and visible Snapshots from
  a hidden child process.
- Thirty Tab presses on each of the four launcher pages stayed on visible
  controls, including the new sound button and the multiline-forward editor.
- Linux race tests, Linux vet, Windows cross-build, Windows vet with the
  repository's `-unsafeptr=false` setting, and the three KVM-probe unit tests pass.
- The final candidate-6 native Windows suite passed **352 top-level tests**, with
  **34 environment-dependent skips**, including the new UI and hidden-Snapshots
  regressions. The test working directory contained the repository fixtures.

## Physical guest checks

Candidate 3 launched the existing guest from **Launch Omarchy** with explicit
4-CPU/4096-MiB settings, GPU rendering, existing disk, shared folder and guest SSH.
The child retained those arguments and added `-start`; there was no second menu.
Guest reboot was detected and relaunched by the supervisor. Clean guest poweroff
exited QEMU and the launcher. No failed user services were reported.

The final candidate 6 also opened Snapshots visibly from the pre-boot launcher,
listed the retained snapshot, returned control after closing it, and launched the
existing GPU desktop from **Launch Omarchy**. Its normal-user nested KVM probe
passed again, with the system reporting `running` and no failed user services.
No snapshot was created, deleted or restored during this UI acceptance pass.
The final candidate retained the large-file and Unicode-file hashes below and
powered off cleanly. No test launcher or QEMU process remained at completion;
Windows itself was left running and signed in.

The prior 128 MiB file retained SHA-256
`e1f7ca7882b27e4a25e635f26c0d4e8788b17bbeec02c0a2c44d77189765cd34`.
The small transfer files retained
`1ae225002124966f47a5dbcb06818a71822c1154e12ed55f42bcfcafb8ab968e`,
and the Unicode filename fixture retained
`0682c5f2076f099c34cfdd15a9e063849ed437a49677e6fcc5b4198c76575be5`.
These match the earlier physical acceptance fixtures.

The KVM probe passed as the normal Omarchy user, then passed again after guest
reboot: API 12, VM/vCPU creation, execution of an actual instruction and
`KVM_EXIT_HLT` (5). The runtime used `q35,accel=whpx`, `-cpu host`, and no
`kernel-irqchip=off` fallback. No nested-refusal warning was present in that run.
This establishes minimal nested KVM on this combination, not a real nested
distribution boot or support on all Windows hosts. No nested QEMU executable was
installed in this guest. The same probe also passed on the Linux development host.

## Failures discovered and resolved

- Candidate 1's About process existed but its window was hidden. Its first
  ShowWindow inherited the child process's `SW_HIDE` startup flag. The window now
  explicitly shows itself; a hidden-startup subprocess regression passes.
- The existing Settings message loop did not activate Close or save text fields
  with Enter. Native Enter dispatch now respects focused push buttons and the
  Save/Launch action, while leaving the multiline forward editor alone.
- The pre-boot recovery pass exposed an invisible Snapshots window in candidate
  5. It held the parent disabled while waiting for input. Snapshots now explicitly
  shows itself. Internal GUI child launches use `CREATE_NO_WINDOW` to suppress
  consoles without passing `SW_HIDE` to their actual windows.
- The first targeted version-resource test lacked repository fixtures in its
  working directory. Supplying the source/resource files fixed the harness.
- One candidate-4 full-suite run hit a Windows sharing violation deleting a
  temporary owned `.lnk` in `TestUnicodeShortcutOwnership`. Candidate 3 and the
  later candidate-5 and final candidate-6 full suites passed that test. The holder was not identified;
  this is retained as an intermittent test observation, not a diagnosed cause
  or a shortcut-deletion fix.

## Remaining boundaries

Windows Hello sudo, live host audio endpoint routing, true trackpad pinch and
bridged networking remain unimplemented. Their controls must not be presented as
working based on this launcher work. The available laptop has no MSYS2 runtime
build toolchain; audio/pinch changes need a rebuilt, validated QEMU runtime through
the documented build process, not unsupported command-line flags.

No new sleep/wake, mixed-DPI, Intel/NVIDIA, Windows 10, full installation move,
destructive snapshot rollback, real nested distribution boot, production signing,
public update chain, or release publication is claimed here. Earlier evidence
for those areas retains its original scope.

Private logs and candidate binaries are retained under
`~/Documents/Codex/2026-09-21/try-omarchy-parity/`; remote harness scripts and binaries
are under `D:\TryOmarchy-Parity-20260921`. Desktop screenshots remain private.
