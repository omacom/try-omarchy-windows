# Third-party notices

Try Omarchy for Windows builds and redistributes third-party components under
their own licenses. The repository's MIT license applies only to this project's
original code.

- **Omarchy** — pinned from `basecamp/omarchy` at the v4.0.3 release tag; MIT.
  Its license is copied into every guest artifact.
- **QEMU** — GPL-2.0 and other component licenses. The runtime builds QEMU from
  the WINQ-EMU fork (`cmspam/winq-emu-qemu`) based on upstream v11.0.0. Because
  GPL-2.0 applies, the corresponding source and build recipe are published
  alongside each runtime as `winq-emu-alpha10-source.zip`.
- **virglrenderer** — MIT. Built from the WINQ-EMU fork
  (`cmspam/winq-emu-virglrenderer`) based on upstream 1.3.0.
- **WINQ-EMU runtime** — the portable Windows QEMU runtime by cmspam
  (`cmspam/winq-emu`). It carries the QEMU and virglrenderer builds above plus
  the MSYS2 libraries they link. It is redistributed as
  `winq-emu-alpha10-portable.zip`.
- **MSYS2 runtime libraries** — SDL2, libslirp, GLib, Pixman, libusb, zlib, and
  the other dependencies of the QEMU build retain their respective upstream
  licenses. The portable archive includes the MSYS2 package inventory, per-file
  hashes, and licenses.
- **Arch Linux x86_64 packages** — each package retains its own license. The
  generated package transaction is recorded in the guest build's package lock.
- **Hyprland** — BSD-3-Clause; pinned by the Omarchy release and shipped through
  the guest package transaction.
- **yay** — GPL-3.0-or-later; shipped from the Omarchy repository with the
  `base-devel` toolchain.
- **ttfx** — MIT; ships the Omarchy screensavers.
- **Voxtype** — MIT; packaged for Omarchy's optional dictation installer.
- **fcitx5** — LGPL-2.1-or-later; shipped so Omarchy's input-method unit runs.
- **klauspost/compress** — BSD-3-Clause; the launcher's only Go dependency,
  used for guest payload compression.
- **1Password** — proprietary software not redistributed by Try Omarchy. When a
  user explicitly invokes its optional installer, the guest resolves the current
  vendor release and CLI recipe after the factory build.
- **Vivaldi** — proprietary software not redistributed by Try Omarchy. When a
  user explicitly selects Vivaldi, the guest repackages the verified official
  build as a Pacman-owned local package. Vendor terms apply; see the Linux
  distribution integration information at
  <https://vivaldi.com/partners/linux/>.
- **dockur/windows** — MIT; used only as a development and test environment, not
  redistributed.

See `guest-build/source.lock.json`, `guest-build/runtime.lock.json`,
`runtime-build/sources.lock.json`, and `scripts/release/` for exact source
identities and checksums. Before distributing a release, follow
[docs/RELEASING.md](docs/RELEASING.md) and audit the assembled bundle's notices
and corresponding-source obligations.
