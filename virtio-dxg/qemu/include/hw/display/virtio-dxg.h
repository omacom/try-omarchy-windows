/*
 * virtio-dxg: Windows host GPU paravirtualization for Linux guests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_VIRTIO_DXG_H
#define HW_VIRTIO_DXG_H

#include "hw/virtio/virtio.h"
#include "qemu/uuid.h"
#include "system/memory.h"
#include "virtio_dxg.h"
#include "dxghost.h"

#define TYPE_VIRTIO_DXG "virtio-dxg-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIODxg, VIRTIO_DXG)

/* PCI BAR holding the IO space, as virtio-gpu uses BAR 4 for hostmem. */
#define DXG_IOSPACE_BAR 4

typedef struct VirtIODxgState VirtIODxgState;

struct VirtIODxg {
    VirtIODevice parent_obj;

    VirtQueue *ctrl_vq;
    VirtQueue *event_vq;
    struct dxghost *host;
    VirtIODxgState *state;
    PCIDevice *pci;
    MemoryRegion iospace;
    uint32_t guest_caps;
    bool stats;                 /* DXGHOST_STATS */

    /* Properties */
    uint64_t iospace_size;
    uint32_t luid_low;
    int32_t luid_high;
    char *umd_name;
    QemuUUID adapter_uuid;
};

#endif
