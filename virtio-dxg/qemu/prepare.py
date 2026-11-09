#!/usr/bin/env python3
"""Adds the virtio-dxg device to a WINQ-EMU QEMU source tree.

    python virtio-dxg/qemu/prepare.py <qemu-source>

Copies the device, the dxghost library and the shared protocol headers into
hw/display/, and builds them only for Windows hosts. Composes with
experimental/qemu/prepare_qemu.py and the official runtime patches: it
touches no existing QEMU source except by appending to
hw/display/meson.build.
"""
import pathlib
import shutil
import sys

here = pathlib.Path(__file__).resolve().parent
root = here.parent

MESON = """
# virtio-dxg: Windows host GPU paravirtualization (dxgkrnl over virtio)
if host_os == 'windows'
  virtio_dxg_inc = include_directories('virtio-dxg/protocol',
                                       'virtio-dxg/host/include')
  libdxghost = static_library('dxghost',
    files('virtio-dxg/host/src/dxghost.c', 'virtio-dxg/host/src/objects.c',
          'virtio-dxg/host/src/commands.c', 'virtio-dxg/host/src/kmt.c'),
    include_directories: include_directories('virtio-dxg/host/src/compat'),
    # Built against the guest's Linux headers with its own warning set.
    c_args: ['-Wno-error', '-Wno-missing-prototypes', '-Wno-shadow'],
    build_by_default: false)
  system_ss.add(when: 'CONFIG_VIRTIO_PCI', if_true: [
    files('virtio-dxg.c'),
    declare_dependency(link_with: libdxghost,
                       include_directories: virtio_dxg_inc)])
endif
"""
MARKER = "# virtio-dxg: Windows host GPU paravirtualization"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    qemu = pathlib.Path(sys.argv[1])
    display = qemu / "hw/display"
    if not (display / "meson.build").exists():
        sys.exit(f"{qemu} is not a QEMU source tree")

    shutil.copyfile(here / "hw/display/virtio-dxg.c", display / "virtio-dxg.c")
    shutil.copyfile(here / "include/hw/display/virtio-dxg.h",
                    qemu / "include/hw/display/virtio-dxg.h")

    tree = display / "virtio-dxg"
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(root / "protocol", tree / "protocol")
    shutil.copytree(root / "host/include", tree / "host/include")
    shutil.copytree(root / "host/src", tree / "host/src")
    # dxghost compiles against the guest's own message definitions.
    (tree / "guest/dxgkrnl").mkdir(parents=True)
    shutil.copyfile(root / "guest/dxgkrnl/dxgvmbus.h",
                    tree / "guest/dxgkrnl/dxgvmbus.h")
    shutil.copytree(root / "guest/include", tree / "guest/include")

    meson = display / "meson.build"
    text = meson.read_text()
    if MARKER not in text:
        meson.write_text(text + MESON, newline="\n")

    # virtio core names devices by ID from a table and asserts the ID is in
    # it; virtio-dxg's ID is past the last standard one.
    virtio = qemu / "hw/virtio/virtio.c"
    text = virtio.read_text()
    entry = '    [62 /* VIRTIO_ID_DXG */] = "virtio-dxg",\n'
    if entry not in text:
        anchor = '    [VIRTIO_ID_SPI] = "virtio-spi"\n};'
        if anchor not in text:
            sys.exit("hw/virtio/virtio.c: device name table not found")
        text = text.replace(anchor, '    [VIRTIO_ID_SPI] = "virtio-spi",\n'
                            + entry + '};')
        virtio.write_text(text, newline="\n")

    # QMP's virtio introspection (x-query-virtio-status) decodes features
    # per device ID and asserts on unknown IDs: one query killed the VM.
    # virtio-dxg has no device feature bits.
    qmp = qemu / "hw/virtio/virtio-qmp.c"
    text = qmp.read_text()
    entry = '    case 62: /* VIRTIO_ID_DXG */\n'
    if entry not in text:
        anchor = '    case VIRTIO_ID_AUDIO_POLICY:\n        break;'
        if anchor not in text:
            sys.exit("hw/virtio/virtio-qmp.c: featureless device list not found")
        text = text.replace(anchor, entry + anchor)
        qmp.write_text(text, newline="\n")
    patch_virtio_gpu(qemu)
    print(f"virtio-dxg added to {qemu}")


# Zero-copy scanout: virtio-gpu 2D asks virtio-dxg whether a resource's
# image is a host GPU texture (include/hw/display/virtio-gpu-scanout.h).
# Without virtio-dxg the hooks pointer stays NULL and nothing changes.
GPU_PATCHES = [
    ('#include "hw/virtio/virtio-gpu-pixman.h"\n',
     '#include "hw/virtio/virtio-gpu-pixman.h"\n'
     '#include "hw/display/virtio-gpu-scanout.h"\n'),
    ('void virtio_gpu_disable_scanout(VirtIOGPU *g, int scanout_id)\n{\n',
     '/* virtio-dxg: zero-copy scanout (hw/display/virtio-gpu-scanout.h). */\n'
     'const VirtIOGPUScanoutHooks *virtio_gpu_scanout_hooks;\n'
     '/* Scanouts that show a texture rather than a surface. */\n'
     'static bool virtio_gpu_scanout_texture[VIRTIO_GPU_MAX_SCANOUTS];\n\n'
     'void virtio_gpu_disable_scanout(VirtIOGPU *g, int scanout_id)\n{\n'),
    ('    dpy_gfx_replace_surface(scanout->con, NULL);\n'
     '    scanout->resource_id = 0;\n',
     '    dpy_gfx_replace_surface(scanout->con, NULL);\n'
     '    virtio_gpu_scanout_texture[scanout_id] = false;\n'
     '    scanout->resource_id = 0;\n'),
    ('    qemu_pixman_image_unref(res->image);\n'
     '    virtio_gpu_cleanup_mapping(g, res);\n',
     '    if (virtio_gpu_scanout_hooks) {\n'
     '        virtio_gpu_scanout_hooks->forget(res->resource_id);\n'
     '    }\n'
     '    qemu_pixman_image_unref(res->image);\n'
     '    virtio_gpu_cleanup_mapping(g, res);\n'),
    # The image is on the GPU: copying guest memory would be wasted.
    ('    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);\n\n'
     '    res = virtio_gpu_find_check_resource(g, t2d.resource_id, true,\n'
     '                                         __func__, &cmd->error);\n'
     '    if (!res || res->blob) {\n'
     '        return;\n'
     '    }\n',
     '    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);\n\n'
     '    res = virtio_gpu_find_check_resource(g, t2d.resource_id, true,\n'
     '                                         __func__, &cmd->error);\n'
     '    if (!res || res->blob) {\n'
     '        return;\n'
     '    }\n'
     '    if (virtio_gpu_scanout_hooks &&\n'
     '        virtio_gpu_scanout_hooks->linked(res->resource_id)) {\n'
     '        return;\n'
     '    }\n'),
    ('    /* create a surface for this scanout */\n',
     '    if (!res->blob && virtio_gpu_scanout_hooks &&\n'
     '        virtio_gpu_scanout_hooks->show(scanout->con, res->resource_id,\n'
     '                                       r->x, r->y, r->width,\n'
     '                                       r->height)) {\n'
     '        /* A texture, presented on the GPU: no surface, no upload. */\n'
     '        virtio_gpu_scanout_texture[scanout_id] = true;\n'
     '        virtio_gpu_update_scanout(g, scanout_id, res, fb, r);\n'
     '        return true;\n'
     '    }\n'
     '    if (virtio_gpu_scanout_texture[scanout_id]) {\n'
     '        /* Back to memory: the console needs its surface again. */\n'
     '        virtio_gpu_scanout_texture[scanout_id] = false;\n'
     '        scanout->ds = NULL;\n'
     '    }\n\n'
     '    /* create a surface for this scanout */\n'),
    ('        scanout = &g->parent_obj.scanout[i];\n\n'
     '        qemu_rect_init(&rect, scanout->x, scanout->y,\n',
     '        scanout = &g->parent_obj.scanout[i];\n'
     '        if (virtio_gpu_scanout_texture[i]) {\n'
     '            virtio_gpu_scanout_hooks->flush(scanout->con,\n'
     '                                            res->resource_id,\n'
     '                                            scanout->width,\n'
     '                                            scanout->height);\n'
     '            continue;\n'
     '        }\n\n'
     '        qemu_rect_init(&rect, scanout->x, scanout->y,\n'),
]


# The guest's display mode follows the host monitor (see virtio-dxg.c,
# dxg_follow_refresh): handle_windowevent is left alone, as the runtime's own
# tests compile it from this file.
SDL_PATCHES = []
# Input is read when SDL's display refresh runs: every 10 ms while busy and
# every 30 ms after a quiet moment, so the pointer moved at 100 Hz (33 Hz at
# first) on any display. It now runs once per display frame (4 ms at
# 240 Hz) while busy and every two frames when quiet.
SDL_PATCHES += [
    ('                scon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;\n',
     '                scon->dcl.update_interval = SDL2_REFRESH_INTERVAL_QUIET;\n'),
]
SDL_HEADER_PATCHES = [
    ('#define SDL2_REFRESH_INTERVAL_BUSY 10\n',
     '/*\n'
     ' * Display and input polling: one display frame while busy (4 ms at\n'
     ' * 240 Hz, 10 ms at most), two when quiet.\n'
     ' */\n'
     '#define SDL2_REFRESH_INTERVAL_BUSY \\\n'
     '    (qemu_edid_refresh_rate_mhz >= 100000 ? \\\n'
     '     MAX(1000000 / qemu_edid_refresh_rate_mhz, 1) : 10)\n'
     '#define SDL2_REFRESH_INTERVAL_QUIET (2 * SDL2_REFRESH_INTERVAL_BUSY)\n'),
]


def patch_file(path, patches, marker):
    text = path.read_text()
    if marker in text:
        return
    for anchor, replacement in patches:
        if text.count(anchor) != 1:
            sys.exit(f"{path}: anchor not found once:\n{anchor}")
        text = text.replace(anchor, replacement)
    path.write_text(text, newline="\n")


def patch_sdl(qemu):
    patch_file(qemu / "ui/sdl2.c", SDL_PATCHES, "SDL2_REFRESH_INTERVAL_QUIET")
    patch_file(qemu / "include/ui/sdl2.h", SDL_HEADER_PATCHES,
               "SDL2_REFRESH_INTERVAL_QUIET")


def patch_virtio_gpu(qemu):
    patch_sdl(qemu)
    shutil.copyfile(here / "include/hw/display/virtio-gpu-scanout.h",
                    qemu / "include/hw/display/virtio-gpu-scanout.h")
    path = qemu / "hw/display/virtio-gpu.c"
    text = path.read_text()
    if "virtio_gpu_scanout_hooks" in text:
        return
    for anchor, replacement in GPU_PATCHES:
        if text.count(anchor) != 1:
            sys.exit(f"hw/display/virtio-gpu.c: anchor not found once:\n{anchor}")
        text = text.replace(anchor, replacement)
    path.write_text(text, newline="\n")


if __name__ == "__main__":
    main()
