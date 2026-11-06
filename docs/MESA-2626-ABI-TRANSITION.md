# Mesa 26.2.6 ABI transition review

The full five-library graphics graph compiles at `f95e3b5` and `98aa0cf`.
Seven recipe controls, twenty CPU cases using the actual fence functions and
fourteen loader-alias controls pass. The strict historical ABI gate rejects
gallium's new export version tags, so those jobs produce no accepted graphics
component archive. Dozen builds separately and passes its file controls.

The [completed ABI observation job](https://github.com/7Wdev/try-omarchy-windows/actions/runs/42209325621/job/127863781745)
records all five libraries' dynamic export metadata before that gate. Comparing
it with the pinned, previously produced 26.0.8 component shows:

- All **527 exported names** retain their ELF symbol type, binding, visibility
  and default-version flag. No symbols are added or removed.
- Gallium's **83 version tags** change from `libgallium-26.0.8.so` to
  `libgallium-26.2.6.so`. Upstream uses `--default-symver`, which incorporates
  the library filename. Applying only that transformation to the old export
  map independently predicts the exact observed new metadata hash.
- EGL, GLES2 and the GBM backend now depend on `libgallium-26.2.6.so`. All other
  dependency names and their order remain unchanged across the five libraries.

[Artifact pins, exact transition and per-library comparison](evidence/mesa-2626-abi-transition-2026-11-06.json).
The versioned contract permits this exact transition and pins all five dependency
lists. Counts, hashes, SONAMEs, generated protocol files and loader-alias checks
remain strict. A new approved build must satisfy that contract before an archive
is accepted. The old recipe and old evidence remain unchanged.

This review reads existing artifact metadata without loading a library. ELF
symbol metadata does not establish C function signatures, object layout, API
conformance, shader correctness or runtime compatibility. A private guest update
must replace the coherent Mesa cohort, handle the new gallium filename, check
the captured guest consumers and stage the matching private NVIDIA libraries.
Ownership/failure regression checks on this release and initialization/reclaim
failure-path review remain required before hardware integration. No guest boot,
display recovery, usable desktop, performance or daily-use acceptance is claimed.
