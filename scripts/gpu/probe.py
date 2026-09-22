#!/usr/bin/env python3
"""Inspect Linux graphics and optionally run small, hardware-only app checks.

No packages, drivers, users or persistent app settings are installed/changed.
Run inside the guest being evaluated. Query results are not workload acceptance.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import tempfile


SOFTWARE = re.compile(r"llvmpipe|lavapipe|softpipe|software rasterizer|swiftshader", re.I)


def accelerated_renderer(name):
    return bool(name.strip()) and not SOFTWARE.search(name)


def run_command(argv, env=None, timeout=30):
    if not shutil.which(argv[0]):
        return {"status": "unavailable", "reason": f"{argv[0]} is not installed"}
    # Capture to a temporary file: verbose failures must not fill process RAM.
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(argv, stdout=log, stderr=log, env=env,
                                   start_new_session=True)
        timed_out = False
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass  # It exited between the deadline and group cleanup.
            process.wait()
        log.seek(0, os.SEEK_END)
        length = log.tell()
        log.seek(max(0, length - 16384))
        return {"status": "timeout" if timed_out else "completed",
                "exitCode": process.returncode,
                "outputTruncated": length > 16384,
                "output": log.read().decode("utf-8", errors="replace")}


def field(output, label):
    match = re.search(r"^[ \t]*" + re.escape(label) + r"[ \t]*[:=][ \t]*([^\r\n]+)$", output, re.M)
    return match.group(1).strip() if match else ""


def classify_gl(result):
    result = dict(result)
    output = result.get("output", "")
    result["renderer"] = field(output, "OpenGL renderer string")
    result["apiVersion"] = field(output, "OpenGL core profile version string")
    if result["status"] == "completed":
        result["status"] = "available" if (result["exitCode"] == 0
            and accelerated_renderer(result["renderer"])
            and re.search(r"Accelerated:\s*yes\b", output, re.I)) else "failed"
    return result


def classify_workload(result, marker, require_adapter=False):
    result = dict(result)
    output = result.get("output", "")
    if result["status"] == "completed":
        passed = result["exitCode"] == 0 and marker in output
        if require_adapter:
            result["renderer"] = field(output, "GPU_ADAPTER")
            passed = passed and accelerated_renderer(result["renderer"])
        result["status"] = "passed" if passed else "failed"
    return result


GODOT_SCENE = '''extends SceneTree
var frames := 0
func _initialize():
    var world := Node3D.new()
    root.add_child(world)
    var mesh := MeshInstance3D.new()
    mesh.mesh = BoxMesh.new()
    world.add_child(mesh)
    var camera := Camera3D.new()
    camera.position = Vector3(0, 0, 4)
    world.add_child(camera)
    camera.current = true
    var light := DirectionalLight3D.new()
    light.rotation_degrees = Vector3(-30, -30, 0)
    world.add_child(light)
    print("GPU_ADAPTER=", RenderingServer.get_video_adapter_name())
func _process(_delta):
    frames += 1
    if frames == 30:
        var image := root.get_texture().get_image()
        if image.is_empty():
            push_error("No rendered image returned")
            quit(2)
            return false
        var center := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
        if center.is_equal_approx(image.get_pixel(0, 0)):
            push_error("Rendered box is indistinguishable from background")
            quit(3)
            return false
        print("GPU_3D_READBACK_PASSED")
        quit(0)
    return false
'''


def blender_script(backend, destination):
    # Disable all CPU devices; device enumeration alone is not a render test.
    return f'''import bpy, json
prefs = bpy.context.preferences.addons['cycles'].preferences
prefs.compute_device_type = {backend!r}
prefs.refresh_devices()
print('GPU_DEVICES=' + json.dumps([(d.name, d.type) for d in prefs.devices]), flush=True)
if not any(d.type == {backend!r} for d in prefs.devices):
    raise RuntimeError('Requested GPU backend unavailable; refusing CPU fallback')
for d in prefs.devices:
    d.use = d.type == {backend!r}
s = bpy.context.scene
s.render.engine = 'CYCLES'
s.cycles.device = 'GPU'
s.cycles.samples = 4
s.render.resolution_x = 64
s.render.resolution_y = 64
s.render.resolution_percentage = 100
s.render.filepath = {str(destination)!r}
bpy.ops.render.render(write_still=True)
print('GPU_RENDER_PASSED={backend}', flush=True)
'''


def probe(backends, godot):
    env = os.environ.copy()
    dxg = Path("/dev/dxg").exists()
    if dxg:
        # Scope WSL choices to child processes, never /etc/environment.
        env["GALLIUM_DRIVER"] = "d3d12"
        paths = ["/usr/lib/wsl/lib", env.get("LD_LIBRARY_PATH", "")]
        env["LD_LIBRARY_PATH"] = ":".join(p for p in paths if p)
    report = {"schemaVersion": 1, "kernel": platform.release(),
              "architecture": platform.machine(), "dxgPresent": dxg,
              "opengl": classify_gl(run_command(["glxinfo", "-B"], env)),
              "vulkan": run_command(["vulkaninfo", "--summary"], env),
              "workloads": {}}
    vk = report["vulkan"]
    if vk["status"] == "completed":
        vk["status"] = "query-succeeded" if vk["exitCode"] == 0 else "failed"
    vk["nonConformantWarning"] = "not a conformant" in vk.get("output", "").lower()
    with tempfile.TemporaryDirectory(prefix="omarchy-gpu-") as directory:
        root = Path(directory)
        for backend in dict.fromkeys(backends):
            script = root / "blender.py"
            script.write_text(blender_script(backend, root / "render.png"), encoding="utf-8")
            result = run_command(["blender", "--factory-startup", "--background",
                                  "--python-exit-code", "7", "--python", str(script)], env, 120)
            report["workloads"]["blender-" + backend.lower()] = classify_workload(
                result, "GPU_RENDER_PASSED=" + backend)
        if godot:
            (root / "project.godot").write_text('[application]\nconfig/name="Omarchy GPU Check"\n')
            (root / "scene.gd").write_text(GODOT_SCENE)
            for renderer in ("gl_compatibility", "forward_plus"):
                result = run_command(["godot", "--path", str(root), "--script", "res://scene.gd",
                                      "--rendering-method", renderer, "--audio-driver", "Dummy"], env)
                report["workloads"]["godot-" + renderer] = classify_workload(
                    result, "GPU_3D_READBACK_PASSED", require_adapter=True)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", action="append", default=[],
                        choices=["CUDA", "OPTIX", "HIP", "ONEAPI"],
                        help="Render a tiny Cycles scene with this backend; repeatable")
    parser.add_argument("--godot", action="store_true", help="Open two brief 3D render windows")
    options = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Run inside the Linux guest or WSL distribution being tested")
    print(json.dumps(probe(options.blender, options.godot), indent=2))
