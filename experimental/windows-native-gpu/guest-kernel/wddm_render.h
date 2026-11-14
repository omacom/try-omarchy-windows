/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef WDDM_RENDER_H
#define WDDM_RENDER_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define WDDM_RENDER_VERSION 1
#define WDDM_RENDER_MAGIC 0x3152444d444457ULL
#define WDDM_RENDER_NATIVE_ROUTE 1

/* Identity for the opt-in D3D12 Gallium route. This does not assert that the
 * userspace broker/Windows backend is alive, or advertise KMS/GEM support. */
struct wddm_render_info {
    __u32 version, flags;
    __u64 signature;
    __u32 max_width, max_height, reserved[2];
};
#define DRM_IOCTL_WDDM_RENDER_INFO _IOWR('d', 0x40, struct wddm_render_info)
#endif
