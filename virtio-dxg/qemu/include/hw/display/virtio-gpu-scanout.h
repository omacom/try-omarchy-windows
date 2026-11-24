/*
 * Zero-copy scanout of virtio-gpu 2D resources: another device (virtio-dxg)
 * can hold a resource's image as a host GPU texture, which the display then
 * shows instead of the resource's guest memory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_VIRTIO_GPU_SCANOUT_H
#define HW_VIRTIO_GPU_SCANOUT_H

#include "ui/console.h"

typedef struct VirtIOGPUScanoutHooks {
    /* The resource's image is a texture: guest memory need not be copied. */
    bool (*linked)(uint32_t resource_id);
    /*
     * Shows the resource on con from its texture (dpy_gl_scanout_texture).
     * False when it has none or con cannot show textures: virtio-gpu then
     * uses the resource's memory as usual.
     */
    bool (*show)(QemuConsole *con, uint32_t resource_id,
                 uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    /* A frame of a shown resource is ready (dpy_gl_update). */
    void (*flush)(QemuConsole *con, uint32_t resource_id,
                  uint32_t width, uint32_t height);
    /* The resource is gone. */
    void (*forget)(uint32_t resource_id);
} VirtIOGPUScanoutHooks;

/* Set by the device that provides textures; BQL. */
extern const VirtIOGPUScanoutHooks *virtio_gpu_scanout_hooks;

#endif
