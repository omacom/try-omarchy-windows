# Guarded Mesa in the normal Omarchy guest candidate

Added November 5, 2026. A completed CPU-control run for the current revision
and private candidate receipt are required before claiming an assembled
candidate. This recipe alone authorizes no VM or GPU test.

The last normal-root image contains the earlier Mesa runtime. The public
component builds now include the uploader ownership fix and constant-buffer
allocation guard, but compiling those components does not put them into that
guest. The new [candidate builder](../experimental/windows-native-gpu/mesa/prepare_guest_mesa_candidate.py)
updates the private initramfs as archive data, without executing a guest ELF,
invoking `ldd`, starting WSL or mounting a disk.

The update is the complete five-library Mesa group plus six loader aliases.
The normal-root image stores those aliases as regular copies; the builder
materializes the checked component symlinks into those existing eleven files.
Names, modes, owners, member order and other header fields stay unchanged. Every
other payload, including official input configuration, guest modules, broker,
shim and private NVIDIA runtime, must retain its exact hash and headers.
The original image is opened read-only and its hash is rechecked.

Before producing a candidate, the builder requires:

- The pinned component tar hash and head, the current guarded source recipe,
  and all five libraries matching the existing 527-export component contract.
- Matching old/new Mesa SONAME and export-interface metadata, and consistent
  copies of all six old loader aliases.
- A bounded, complete manifest-matching `newc` input, with canonical names,
  no duplicate entries, zero padding and the expected read-only private profile.
- Captured providers for the declared ELF `DT_NEEDED` closure, required version
  nodes and strong symbol imports under the explicit runtime search roots.
- A complete readback of the new compressed image against the updated manifest
  and original headers before promoting the candidate filename.

The [ELF inspector](../experimental/windows-native-gpu/mesa/guest_elf_abi.py)
operates on bytes. It does not load Linux libraries or consult Windows/host
library paths. Symbol indexes avoid scanning every export for each import.
The input has a 384 MiB expanded bound and 8,192-member limit; captured provider
bytes are capped at 128 MiB, with ordinary provider files limited to 16 MiB.
Larger required providers cause refusal. No GPU resource budget is increased.

This is a static admission check, not a replacement for the Linux dynamic
loader. Runtime `dlopen`, preloads, relocations, symbol implementation/object
layout and the actual process environment remain unverified. Optional weak
imports are recorded without requiring definitions; provider version nodes are
still required. RPATH/RUNPATH providers are rejected rather than modeled.

Remote CI defines fourteen data controls using genuine compiled Mesa, pinned
Wayland and runner-system ELF bytes in a synthetic archive. One valid update
and thirteen rejections cover base/component pins, missing/nonregular/wrong
aliases, incompatible exports, missing providers/symbols/versions, duplicate
CPIO names, changed component bytes and tar traversal. The input/private marker
payloads are synthetic; no proprietary runtime or actual VM image is uploaded.
No control results are claimed before the run completes.

The real candidate and all private provenance stay local. Its manifest records
the component source and compiler receipt, parent hash, exact changed payloads
and static-check scope. It remains `guestBooted=false`, `normalDesktopVerified=false`
and `dailyUseAccepted=false`. This does not prove the Quickshell crash resolved
or establish recovery from the host display incident. Any later hardware test
requires a separate concrete approval and disclosure of shared-driver risk.

Normal startup, responsive official input, sustained browsing/multiple apps
through 4K, bounded resources and cleanup/recovery remain acceptance work.
CUDA, Vulkan presentation, video, DLSS, hardware ray tracing and the >=93%
matched-native performance target remain unfinished. Counts stay **4/8, 0/4, 3/3**.
