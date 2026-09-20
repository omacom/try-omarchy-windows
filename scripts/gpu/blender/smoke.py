"""Run with Blender's --python option in a disposable graphical session.

Creates a small scene, renders Workbench/Eevee/Cycles CPU, and checks saved data.
Pass -- --output DIRECTORY. This intentionally changes the current scene.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import traceback

import bpy
import gpu


def check():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    output = parser.parse_args(arguments).output
    output.mkdir(parents=True, exist_ok=True)
    details = {"renderer": gpu.platform.renderer_get(),
               "vendor": gpu.platform.vendor_get(), "version": gpu.platform.version_get(),
               "backend": gpu.platform.backend_type_get(), "blender": bpy.app.version_string}
    print("GPU_DETAILS=" + json.dumps(details), flush=True)
    if any(name in details["renderer"].lower() for name in ("llvmpipe", "softpipe", "lavapipe", "swiftshader")):
        raise RuntimeError("Software rendering does not satisfy this GPU check")
    bpy.ops.wm.read_factory_settings(use_empty=False)
    scene = bpy.context.scene
    cube = bpy.data.objects["Cube"]
    cube.rotation_euler.z = 0.3
    cube["omarchy_acceptance"] = "saved scene data"
    scene.render.resolution_x = 320
    scene.render.resolution_y = 240
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 8
    results = []
    for engine in ("BLENDER_WORKBENCH", "BLENDER_EEVEE", "CYCLES"):
        scene.render.engine = engine
        target = output / (engine.lower() + ".png")
        scene.render.filepath = str(target)
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(str(target), check_existing=False)
        try:
            if tuple(image.size) != (320, 240):
                raise RuntimeError(f"Unexpected image size: {engine}")
            pixels = list(image.pixels)
            red = pixels[::4]
            if max(red) - min(red) < 0.05:
                raise RuntimeError(f"Blank or uniform image: {engine}")
            # The factory cube must be centered against a uniform background.
            # Pixel variance alone accepts corrupt GPU readbacks as an image.
            corners = [red[y * 320 + x] for x, y in ((8, 8), (311, 8), (8, 231), (311, 231))]
            if max(corners) - min(corners) > 0.10:
                raise RuntimeError(f"Corrupt background/camera framing: {engine}")
            center = [red[y * 320 + x] for y in range(70, 170, 10) for x in range(110, 210, 10)]
            if max(center) - sum(corners) / 4 < 0.04:
                raise RuntimeError(f"Expected centered cube is missing: {engine}")
        finally:
            bpy.data.images.remove(image)
        results.append(engine)
        print("RENDER_PASSED=" + engine, flush=True)
    scene.render.engine = "BLENDER_EEVEE"
    target = output / "omarchy-scene.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(target), copy=True)
    with bpy.data.libraries.load(str(target), link=False) as (source, destination):
        if "Cube" not in source.objects:
            raise RuntimeError("Saved scene is missing its cube")
        destination.objects = ["Cube"]
    restored = destination.objects[0]
    if restored.get("omarchy_acceptance") != "saved scene data" or abs(restored.rotation_euler.z - 0.3) > 0.001:
        raise RuntimeError("Saved object did not retain its edited data")
    bpy.data.objects.remove(restored)
    details["passed"] = results + ["save-and-load-edited-object"]
    (output / "result.json").write_text(json.dumps(details, indent=2) + "\n", encoding="utf-8")
    print("BLENDER_APPLICATION_CHECK_PASSED", flush=True)
    bpy.ops.wm.quit_blender()


def run():
    try:
        check()
    except Exception:
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(1)  # Timer exceptions otherwise leave Blender open with exit status 0.


bpy.app.timers.register(run, first_interval=3)
