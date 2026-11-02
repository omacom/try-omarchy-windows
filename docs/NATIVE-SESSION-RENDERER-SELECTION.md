# Native session renderer selection

**November 2, 2026:** the native session now has a pinned Aquamarine factory
adapter that keeps boot VGA out of desktop renderer selection. The complete
library builds, 17 factory controls pass, and the original factory fails the new
native-isolation control. Normal guest startup and GPU presentation with this
library remain unverified. Counts stay **backend 4/8, desktop 0/4, scene 3/3**.

The normal Hyprland source requests a mandatory headless implementation and
optional DRM/Wayland implementations. The bridge's native output uses that
internal headless implementation to submit visible frames to QEMU's SDL relay.
If the boot VGA device also creates a working DRM implementation, the existing
native allocator rejects the mixed renderer set. The boot console therefore
needs explicit isolation before those implementations are constructed.

The new adapter checks the existing native-render opt-in and the persistent
compositor hook's active-session callback. Only an active native session selects
the isolated implementation. Its extent must be 1920×1080 or 3840×2160, and its
options must contain exactly one mandatory native output implementation. Failed,
unknown, malformed or ambiguous selection fails before device construction.
Missing or inactive session callbacks preserve the earlier diagnostic behavior;
unselected ordinary backend creation also remains unchanged.

This runs once during backend creation. It adds no per-frame operation, repaint
timer, worker or GPU readback. The existing native allocator/device identity
checks remain in place. The existing seat, libseat and libinput startup path
remains responsible for guest input. Windows keyboard, tablet, reserved keys,
SDL events and launcher arguments are unchanged.

Verification uses the **complete production factory function**, extracted from
the pinned source, with mocked framework/device constructors and real Linux
timer descriptors. Seventeen controls cover both supported extents, legacy
selection, missing DRM, missing/duplicate/optional/fallback native output, invalid
mode and extent, constructor ordering, close-on-exec and descriptor retirement.
The complete original factory still passes six legacy controls and is rejected
specifically by the native constructor-isolation assertion.

Eight source guards reject modified, partial, wrong-version, missing-adapter,
symlink and damaged-boundary inputs. Reapplication and the full inverse are exact.
Only `src/backend/Backend.cpp` changes among **79 authored Aquamarine files**;
the other 78 remain byte-identical, including seat/input sources. The complete
pinned Aquamarine 0.15.1 library then builds successfully with all 79 tested
authored source hashes preserved. External dependency warnings remain in the
retained build log. This is build/CPU evidence, not a GPU or input-latency test.

The source adapter must be applied after the existing native-render, native-input
and persistent-session adapters. Normal packaging must install this exact matched
library together with the broker, hook, exec wrapper and guest modules. The normal
provisioning/login flow, application GPU environment, actual same-window GPU
handoff, sustained use and cleanup/recovery still need end-to-end validation.
CUDA, broader OpenGL/Vulkan/video, DLSS, hardware ray tracing and **at least 93%
matched-native performance** remain unverified.

[Structured evidence](evidence/native-session-renderer-selection-2026-11-02.json) ·
[Boot-console source](NATIVE-BOOT-CONSOLE.md) ·
[Normal activation service](ROOT-SESSION-SERVICE.md).
