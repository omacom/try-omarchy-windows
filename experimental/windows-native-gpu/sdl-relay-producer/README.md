# Cross-process QEMU SDL GPU control

This separate NVIDIA D3D12 producer exports a bounded 3840×2160 RGBA8 texture and
independent named producer/consumer fences. The QEMU relay imports them using a
fresh session/generation. Original NT export handles close after import, before
any frame. Every GPU clear and consumer fence must retire; cleanup retains no
owned native handles. The control does not install a driver or boot a guest disk.

Build with Visual Studio C++ tools and the Windows SDK:

    .\build.ps1 -OutputDirectory C:\path\to\fresh-build

The QEMU artifact must be built from the matching graphics relay source. Run
the controller on Windows with absolute paths to the extracted candidate,
producer build and fresh output directory:

    python accept_qemu.py --candidate C:\path\to\qemu-candidate --artifact-receipt C:\path\to\artifact-receipt.json --producer-build C:\path\to\fresh-build --output C:\path\to\fresh-control

The receipt must bind the candidate's file hashes to its digest-verified GitHub
artifact and revision. The controller validates that snapshot and captures its
actual corresponding C/header/UI source, rather than attributing an older
binary to the current working tree. Producer and controller source bytes must
stay frozen throughout the run.

The default uses a paused WHPX VM, the existing SDL/OpenGL frontend and official
virtio keyboard/tablet devices. The optional --tcg argument selects TCG. The
frame count is bounded to 2..300. Only the first and last frames request complete
source and actual pre-swap SDL backbuffer readbacks; ordinary frames request
only GPU copies. No physical keyboard/mouse input is injected.

The --bgra argument probes a BGRA8 D3D12 producer resource. It is intended to
check real compositor format compatibility; neither successful import nor a
protocol acknowledgement proves that its channel order is correct. The same
independent complete-pixel and fence checks remain required.

The controller checks ownership, adapter identity, generation/order rejection,
separate GPU fence retirement and natural shutdown. It preserves logs and
rejected pixels on failure. **Independent complete-pixel auditing is still
required** before accepting a run; the controller's protocol result alone does
not establish rendering correctness, physical scanout or performance.

This is an engineering component for integrating real guest buffers into the
official window. It does not prove an Omarchy guest through SDL, normal launcher
integration, general API support or near-native performance.
