# Resource profiles: Intel / NVIDIA Windows acceptance

Tested 2026-09-20 on a physical Windows 11 Pro PC (build 26200), Intel
Core i9-14900KF (32 logical processors), 64 GB installed RAM, and NVIDIA
GeForce RTX 5080, Windows driver 32.0.16.1692.

Source base: `b1d3b65`, with this resource-profile change applied. The tested
unbranded launcher was built locally with Go 1.27.1; SHA256:
`556df3225b471801e81e0331e745829dcce661b864abc128c0d5829aaa24248c`.
Guest and runtime: existing v0.0.20-preview payloads, without a package or
runtime update. QEMU executable SHA256:
`7b3aad5f42c1e7287ffb80dcbe73882deb8fe797cc55feff0ab8f5e1f53c0643`.
Tests used an independent writable copy of an already provisioned guest.

## Launcher checks

- `go test ./...` passed on Windows, including a run with `QEMU_SYSTEM`
  pointing to the installed runtime to enable native QMP/runtime tests.
- `go vet -unsafeptr=false ./...` passed, matching the Windows CI configuration.
- Built the console-less amd64 launcher with `go build -trimpath -ldflags
  "-H windowsgui -s -w"`.
- Native Settings: Balanced and Maximum performance disable manual fields;
  Manual enables CPU and GiB inputs. Saving 16 CPUs and 24 GiB persisted
  `cpus: 16`, `memoryMiB: 24576`, and the separate Manual profile.
- Maximum performance boot: 26 vCPUs and 44032 MiB requested, sampled Windows
  CPU busy 3.6%, available memory 52398 MiB. Guest `nproc` returned 26 and
  `free -m` reported 43121 MiB usable RAM (kernel overhead is expected).
- Manual boot: 16 vCPUs and 24576 MiB requested; the guest reported 16 CPUs
  and 24021 MiB usable RAM. Both boots reached Hyprland and the guest readiness
  service, with working clipboard and supervisor connections.

The automation host could not use AF_UNIX endpoints under its virtualized
Local AppData path. Testing used an isolated temporary `LOCALAPPDATA` for the
launcher process. No Windows security settings or production IPC code were
changed. This environmental workaround is not claimed as a product fix.

## Application results and failures

Guest versions: Mesa 26.2.3, Godot 4.7.2, Blender 5.2.2 LTS.
Graphics commands used the active session's `DISPLAY=:1`,
`WAYLAND_DISPLAY=wayland-1`, and `XDG_RUNTIME_DIR=/run/user/1000`.

| Check | Observed result |
| --- | --- |
| `glxinfo -B` | Accelerated: yes; renderer `virgl (NVIDIA GeForce RTX 5080/PCIe/SSE2)`; OpenGL core 4.2. |
| Godot Compatibility editor | Opened a temporary empty project, reported the accelerated VirGL device, and completed `--quit-after 5`. |
| Godot Forward+ editor | Reported Vulkan 1.4.343 and `Virtio-GPU Venus (NVIDIA GeForce RTX 5080)`, then failed to finish; QEMU stopped answering and the supervisor terminated it. **Failed**, not Vulkan acceptance. |
| `vulkaninfo --summary` | Failed at `vkCreateImage` with `ERROR_FORMAT_NOT_SUPPORTED`. |
| Blender CPU Cycles render | Factory scene, 64x64, four samples: wrote a PNG and exited successfully. |
| Blender OpenGL viewport | Exited 1 with `EGL_BAD_MATCH`; **failed**. |

The graphics failures occurred with the existing release runtime and guest;
this change neither repairs them nor establishes their root cause. Larger
resource allocations do not make unsupported graphics features available.
Games, sustained application workloads, Blender CUDA/OptiX, VRAM partitioning,
and physical GPU passthrough were not validated. No gaming-performance or
native-GPU-equivalence claim is made.

To reproduce the successful application checks on a disposable guest:

```sh
glxinfo -B
godot --path /tmp/omarchy-godot-smoke --editor \
  --rendering-method gl_compatibility --quit-after 5
blender --factory-startup --background --python-expr \
  'import bpy; s=bpy.context.scene; s.render.engine="CYCLES"; s.cycles.device="CPU"; s.cycles.samples=4; s.render.resolution_x=64; s.render.resolution_y=64; s.render.resolution_percentage=100; s.render.filepath="/tmp/omarchy-blender-smoke.png"; bpy.ops.render.render(write_still=True)'
```

The Godot path must contain a minimal `project.godot`. These are startup/smoke
checks, not performance benchmarks or validation of a user's full project.
