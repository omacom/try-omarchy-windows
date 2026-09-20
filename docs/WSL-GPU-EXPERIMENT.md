# Windows-host GPU access: WSL feasibility

WSL exposes capabilities that the current QEMU graphics path does not, including
NVIDIA CUDA. It is a candidate for GPU application support, **not a working
replacement for the complete Omarchy guest**. There is no WSL backend in the
launcher in this branch. No physical GPU passthrough is implemented.

## Measured on 2026-09-20

Windows 11 Pro 26200; i9-14900KF; RTX 5080; NVIDIA Windows driver 616.92.
A separate `TryOmarchy-GPU-Lab` WSL2 distribution was cloned from an existing
Arch base. The original Arch distribution and QEMU installation were retained.
Only the test distro received packages. No host drivers, Hyper-V policy, security
settings or `.wslconfig` were changed.

Guest versions: WSL kernel 6.18.33.2, Mesa 26.2.3, Blender 5.2.2 LTS,
Godot 4.7.2, Aquamarine 0.15.1.

| Check | Result | Meaning |
| --- | --- | --- |
| OpenGL query | Accelerated D3D12 / RTX 5080, core 4.6 | Hardware renderer available; not an application benchmark. |
| Vulkan query | Dozen, device API 1.2.354 | Explicit non-conformant warning; not full native Vulkan. |
| Blender CUDA | Passed | Factory scene, 64x64, four samples, GPU selected, every CPU device disabled; PNG written and exit 0. |
| Blender OptiX | Failed | Initialization reported error 7805; no OptiX device; test refused CPU fallback and exited 7. |
| Blender viewport | Startup/close passed | Factory scene window opened and exited; no interactive editing or sustained workload validation. |
| Godot OpenGL | Passed | Generated lit 3D box, image readback after 30 process frames, center differs from background, D3D12 RTX adapter selected, exit 0. |
| Godot Forward+ | Passed, limited smoke scope | Same scene through Dozen, exit 0. Does not establish Vulkan conformance or game compatibility. |
| Godot editor | Incomplete | Initial editor startup checks timed out during scanning; runtime-scene checks above were tested separately. |
| Hyprland nested desktop | Failed | Connected to WSLg, then `Wayland backend cannot start: Missing protocols`, `no allocator available`, exit 134. Tested as an unprivileged user. |

The WSL application results are separate from the
[QEMU acceptance results](evidence/RESOURCE-PROFILES-INTEL-NVIDIA-2026-09-20.md).
An app passing in WSL does not make it work in the installed QEMU guest.

## Reproduce on another PC

Use a disposable Linux distro with Python 3 and the applications you want to
test. The probe does not install packages, edit drivers or change app preferences.
For Arch, `mesa-utils` supplies `glxinfo`, `vulkan-tools` supplies `vulkaninfo`,
and `vulkan-dzn` supplies Dozen. Package names and driver availability vary by
distribution. Avoid partial Arch upgrades when installing packages.

From the repository in PowerShell:

```powershell
# Inspect an explicitly selected existing WSL distribution.
scripts/gpu/check-wsl.ps1 -Distribution YourTestDistro -OutputPath gpu-report.json

# NVIDIA example: actual Cycles renders plus two short Godot windows.
scripts/gpu/check-wsl.ps1 -Distribution YourTestDistro `
  -BlenderBackend CUDA,OPTIX -Godot -OutputPath gpu-report.json
```

The guest script also works directly in a Linux guest:

```sh
python3 scripts/gpu/probe.py --blender CUDA --godot
python3 -m unittest discover -s scripts/gpu -p 'test_*.py'
```

`--blender` accepts CUDA, OPTIX, HIP or ONEAPI, and may be repeated. These are
requests to test an installed backend, **not promises that it exists in WSL**.
For example, HIP requires compatible AMD hardware and its supported driver/runtime;
selecting it cannot turn an unsupported GPU into a compatible one.

The probe reports missing tools, failures and timeouts separately. A successful
Vulkan query is called `query-succeeded`, never workload acceptance. Godot results
reject software-renderer names; Blender refuses to render if the requested GPU
backend has no devices. No GPU model, PCI ID, Windows username or driver directory
is hardcoded. Raw reports contain local diagnostic output; inspect before sharing.

## Requirements before shipping a GPU-sharing backend

- Prove the complete Hyprland/Omarchy desktop, input, clipboard and lifecycle.
  WSLg currently fails the tested Hyprland backend requirements. Adding a distro
  launcher would not solve that compositor integration problem.
- Probe capabilities separately: desktop rendering, OpenGL, Vulkan presentation,
  compute and the specific application workload. Never label `nvidia-smi` output
  or an enumerated GPU as successful rendering.
- Detect every adapter and record the adapter actually selected by the application.
  Test NVIDIA, AMD and Intel; laptops with multiple adapters; low-memory systems;
  and missing or incompatible host drivers. Only this NVIDIA machine has been
  physically tested here. Unit tests of vendor names are not hardware validation.
- Keep driver files on the user's machine. WSL supplies the matching host bridge;
  do not bundle this PC's NVIDIA libraries into a release.
- Account for WSL resource limits being shared across its distributions. Existing
  QEMU CPU/RAM controls cannot be applied to WSL as if it were an independent VM.
- Validate actual games and full projects before claiming useful performance.
  There are no game benchmarks or sustained-load results in this report.

Ordinary Hyper-V Linux GPU-PV is another experimental option. The
[NVIDIA community implementation](https://github.com/Ripthulhu/easy-gpu-pv-linux)
reports CUDA and graphics on a different card, but explicitly describes incomplete
Dozen Vulkan and untested presentation. It does not establish a working solution
for this PC or remove the need to test the full desktop. It was not installed.

References: [Microsoft WSLg architecture](https://github.com/microsoft/wslg),
[NVIDIA CUDA on WSL](https://docs.nvidia.com/cuda/wsl-user-guide/index.html),
[Mesa D3D12](https://docs.mesa3d.org/drivers/d3d12.html), and
[Aquamarine shared-memory backend request](https://github.com/hyprwm/aquamarine/issues/228).
