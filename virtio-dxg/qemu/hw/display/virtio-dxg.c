/*
 * virtio-dxg: Windows host GPU paravirtualization for Linux guests.
 *
 * Carries the guest's dxgkrnl messages to dxghost, which executes them with
 * the host GPU driver through D3DKMT. GPU memory the guest touches with the
 * CPU is mapped into the IO-space BAR; commands, fences and resources never
 * pass through QEMU as data.
 *
 * Requests run on lane threads, one ordered lane per guest process (hashed),
 * so a slow call in one guest process never stalls another, and the vCPU
 * that kicked the queue returns to the guest immediately.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/display/virtio-dxg.h"
#include "system/memory.h"
#include "trace.h"

#define DXG_LANES 8
#define DXG_QUEUE_SIZE 256

typedef struct DxgRequest {
    VirtQueueElement *elem;
    uint32_t channel;
    bool async;
    uint8_t *message;
    uint32_t size;
    uint32_t result_size;
    QSIMPLEQ_ENTRY(DxgRequest) next;
} DxgRequest;

typedef struct DxgLane {
    VirtIODxg *dev;
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QSIMPLEQ_HEAD(, DxgRequest) queue;
    bool busy;
    bool quit;
} DxgLane;

typedef struct DxgMapping {
    MemoryRegion mr;
    uint64_t offset;
    uint64_t size;
    QTAILQ_ENTRY(DxgMapping) next;
} DxgMapping;

typedef struct DxgEvent {
    uint32_t size;
    QSIMPLEQ_ENTRY(DxgEvent) next;
    uint8_t packet[];
} DxgEvent;

struct VirtIODxgState {
    DxgLane lanes[DXG_LANES];
    QTAILQ_HEAD(, DxgMapping) mappings;
    QemuMutex event_lock;
    QSIMPLEQ_HEAD(, DxgEvent) events;
    QEMUBH *event_bh;
};

/* --- dxghost callbacks ------------------------------------------------- */

/* Where the guest placed the IO-space BAR, or 0. */
static uint64_t dxg_iospace_gpa(VirtIODxg *d)
{
    pcibus_t addr = d->pci ? pci_get_bar_addr(d->pci, DXG_IOSPACE_BAR) :
                             PCI_BAR_UNMAPPED;

    return addr == PCI_BAR_UNMAPPED ? 0 : addr;
}

/* QOM frees a view once the last flatview reference is gone. */
static void dxg_mapping_free(void *obj)
{
    g_free(container_of(MEMORY_REGION(obj), DxgMapping, mr));
}

static int dxg_map(void *opaque, uint64_t gpa, void *hva, uint64_t size,
                   bool writable)
{
    VirtIODxg *d = opaque;
    g_autofree char *name = NULL;
    uint64_t base;
    DxgMapping *m;
    bool locked = bql_locked();

    if (!locked) {
        bql_lock();
    }
    base = dxg_iospace_gpa(d);
    if (!base || gpa < base || size > d->iospace_size ||
        gpa - base > d->iospace_size - size) {
        if (!locked) {
            bql_unlock();
        }
        return -1;
    }
    m = g_new0(DxgMapping, 1);
    m->offset = gpa - base;
    m->size = size;
    name = g_strdup_printf("virtio-dxg-view-%" PRIx64, m->offset);
    memory_region_init_ram_ptr(&m->mr, OBJECT(d), name, size, hva);
    OBJECT(&m->mr)->free = dxg_mapping_free;
    memory_region_set_readonly(&m->mr, !writable);
    memory_region_add_subregion(&d->iospace, m->offset, &m->mr);
    QTAILQ_INSERT_TAIL(&d->state->mappings, m, next);
    if (!locked) {
        bql_unlock();
    }
    return 0;
}

static void dxg_unmap(void *opaque, uint64_t gpa, uint64_t size)
{
    VirtIODxg *d = opaque;
    uint64_t offset;
    DxgMapping *m;
    bool locked = bql_locked();

    if (!locked) {
        bql_lock();
    }
    offset = gpa - dxg_iospace_gpa(d);
    QTAILQ_FOREACH(m, &d->state->mappings, next) {
        if (m->offset == offset && m->size == size) {
            QTAILQ_REMOVE(&d->state->mappings, m, next);
            memory_region_del_subregion(&d->iospace, &m->mr);
            object_unparent(OBJECT(&m->mr));  /* frees via dxg_mapping_free */
            break;
        }
    }
    if (!locked) {
        bql_unlock();
    }
}

static void dxg_event(void *opaque, const void *packet, uint32_t size)
{
    VirtIODxg *d = opaque;
    DxgEvent *e = g_malloc(sizeof(*e) + size);

    e->size = size;
    memcpy(e->packet, packet, size);
    qemu_mutex_lock(&d->state->event_lock);
    QSIMPLEQ_INSERT_TAIL(&d->state->events, e, next);
    qemu_mutex_unlock(&d->state->event_lock);
    qemu_bh_schedule(d->state->event_bh);
}

static void dxg_log(void *opaque, const char *message)
{
    (void)opaque;
    warn_report("virtio-dxg: %s", message);
}

/* Moves queued host events into guest buffers. Main loop, BQL held. */
static void dxg_flush_events(void *opaque)
{
    VirtIODxg *d = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(d);
    bool notify = false;

    for (;;) {
        VirtQueueElement *elem;
        struct virtio_dxg_event hdr;
        DxgEvent *e;

        qemu_mutex_lock(&d->state->event_lock);
        e = QSIMPLEQ_FIRST(&d->state->events);
        qemu_mutex_unlock(&d->state->event_lock);
        if (!e) {
            break;
        }
        elem = virtqueue_pop(d->event_vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;  /* retried when the guest posts buffers */
        }
        qemu_mutex_lock(&d->state->event_lock);
        QSIMPLEQ_REMOVE_HEAD(&d->state->events, next);
        qemu_mutex_unlock(&d->state->event_lock);
        hdr.size = cpu_to_le32(e->size);
        hdr.reserved = 0;
        iov_from_buf(elem->in_sg, elem->in_num, 0, &hdr, sizeof(hdr));
        iov_from_buf(elem->in_sg, elem->in_num, sizeof(hdr), e->packet,
                     e->size);
        virtqueue_push(d->event_vq, elem, sizeof(hdr) + e->size);
        g_free(elem);
        g_free(e);
        notify = true;
    }
    if (notify) {
        virtio_notify(vdev, d->event_vq);
    }
}

static void dxg_handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
    dxg_flush_events(VIRTIO_DXG(vdev));
}

/* --- Control queue ----------------------------------------------------- */

static void dxg_complete(VirtIODxg *d, DxgRequest *r, uint8_t *result,
                         const struct virtio_dxg_resp *resp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(d);
    size_t written = 0;

    bql_lock();
    if (!r->async) {
        struct virtio_dxg_resp out = {
            .status = cpu_to_le32(resp->status),
            .size = cpu_to_le32(resp->size),
            .ntstatus = cpu_to_le32(resp->ntstatus),
        };

        written = iov_from_buf(r->elem->in_sg, r->elem->in_num, 0, &out,
                               sizeof(out));
        if (resp->size) {
            written += iov_from_buf(r->elem->in_sg, r->elem->in_num,
                                    sizeof(out), result, resp->size);
        }
    }
    virtqueue_push(d->ctrl_vq, r->elem, written);
    virtio_notify(vdev, d->ctrl_vq);
    bql_unlock();
}

static void *dxg_lane_main(void *opaque)
{
    DxgLane *lane = opaque;
    VirtIODxg *d = lane->dev;
    g_autofree uint8_t *result = g_malloc(VIRTIO_DXG_MAX_MESSAGE);

    qemu_mutex_lock(&lane->lock);
    for (;;) {
        DxgRequest *r;
        struct virtio_dxg_resp resp;

        while (!lane->quit && QSIMPLEQ_EMPTY(&lane->queue)) {
            lane->busy = false;
            qemu_cond_signal(&lane->cond);
            qemu_cond_wait(&lane->cond, &lane->lock);
        }
        if (lane->quit) {
            break;
        }
        r = QSIMPLEQ_FIRST(&lane->queue);
        QSIMPLEQ_REMOVE_HEAD(&lane->queue, next);
        lane->busy = true;
        qemu_mutex_unlock(&lane->lock);

        dxghost_execute(d->host, r->channel, r->message, r->size,
                        r->async ? NULL : result, r->result_size, &resp);
        dxg_complete(d, r, result, &resp);
        g_free(r->elem);
        g_free(r->message);
        g_free(r);

        qemu_mutex_lock(&lane->lock);
    }
    qemu_mutex_unlock(&lane->lock);
    return NULL;
}

static void dxg_fail(VirtIODxg *d, VirtQueueElement *elem, uint32_t status)
{
    struct virtio_dxg_resp out = { .status = cpu_to_le32(status) };
    size_t n = iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out));

    virtqueue_push(d->ctrl_vq, elem, n);
    g_free(elem);
}

/* Guest kicked the control queue. vCPU thread or main loop, BQL held. */
static void dxg_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIODxg *d = VIRTIO_DXG(vdev);
    VirtQueueElement *elem;
    bool failed = false;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        struct virtio_dxg_req hdr;
        DxgRequest *r;
        DxgLane *lane;
        size_t in_size = iov_size(elem->in_sg, elem->in_num);

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr)) !=
            sizeof(hdr)) {
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            failed = true;
            continue;
        }
        r = g_new0(DxgRequest, 1);
        r->elem = elem;
        r->channel = le32_to_cpu(hdr.channel);
        r->async = le32_to_cpu(hdr.kind) == VIRTIO_DXG_REQ_ASYNC;
        r->size = le32_to_cpu(hdr.size);
        r->result_size = le32_to_cpu(hdr.result_size);
        if (r->size > VIRTIO_DXG_MAX_MESSAGE ||
            r->result_size > VIRTIO_DXG_MAX_MESSAGE ||
            (!r->async &&
             in_size < sizeof(struct virtio_dxg_resp) + r->result_size)) {
            g_free(r);
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            failed = true;
            continue;
        }
        r->message = g_malloc(r->size);
        if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), r->message,
                       r->size) != r->size) {
            g_free(r->message);
            g_free(r);
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            failed = true;
            continue;
        }
        lane = &d->state->lanes[dxghost_lane(r->message, r->size) %
                                DXG_LANES];
        qemu_mutex_lock(&lane->lock);
        QSIMPLEQ_INSERT_TAIL(&lane->queue, r, next);
        lane->busy = true;
        qemu_cond_signal(&lane->cond);
        qemu_mutex_unlock(&lane->lock);
    }
    if (failed) {
        virtio_notify(vdev, vq);
    }
}

/* Waits until no lane executes a request. BQL is released meanwhile:
 * lanes take it to complete requests and to map memory. */
static void dxg_drain(VirtIODxg *d)
{
    int i;

    bql_unlock();
    for (i = 0; i < DXG_LANES; i++) {
        DxgLane *lane = &d->state->lanes[i];

        qemu_mutex_lock(&lane->lock);
        while (lane->busy || !QSIMPLEQ_EMPTY(&lane->queue)) {
            qemu_cond_wait(&lane->cond, &lane->lock);
        }
        qemu_mutex_unlock(&lane->lock);
    }
    bql_lock();
}

/* --- VirtIO device ----------------------------------------------------- */

static void dxg_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIODxg *d = VIRTIO_DXG(vdev);
    struct virtio_dxg_config c;

    dxghost_device_config(d->host, &c);
    c.vmbus_version = cpu_to_le32(c.vmbus_version);
    c.host_caps = cpu_to_le32(c.host_caps);
    c.guest_caps = cpu_to_le32(d->guest_caps);
    c.flags = cpu_to_le32(c.flags);
    c.vgpu_luid_low = cpu_to_le32(c.vgpu_luid_low);
    c.vgpu_luid_high = cpu_to_le32(c.vgpu_luid_high);
    c.max_message = cpu_to_le32(c.max_message);
    c.event_size = cpu_to_le32(c.event_size);
    memcpy(config, &c, sizeof(c));
}

static void dxg_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIODxg *d = VIRTIO_DXG(vdev);
    struct virtio_dxg_config c;

    memcpy(&c, config, sizeof(c));
    d->guest_caps = le32_to_cpu(c.guest_caps);
}

static uint64_t dxg_get_features(VirtIODevice *vdev, uint64_t features,
                                 Error **errp)
{
    return features;
}

static void dxg_reset(VirtIODevice *vdev)
{
    VirtIODxg *d = VIRTIO_DXG(vdev);
    DxgEvent *e;

    dxg_drain(d);
    dxghost_reset(d->host);  /* unmaps every view through dxg_unmap */
    qemu_mutex_lock(&d->state->event_lock);
    while ((e = QSIMPLEQ_FIRST(&d->state->events))) {
        QSIMPLEQ_REMOVE_HEAD(&d->state->events, next);
        g_free(e);
    }
    qemu_mutex_unlock(&d->state->event_lock);
}

static void dxg_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIODxg *d = VIRTIO_DXG(dev);
    struct dxghost_config config = { 0 };
    struct dxghost_vmm vmm = {
        .opaque = d,
        .map = dxg_map,
        .unmap = dxg_unmap,
        .event = dxg_event,
        .log = dxg_log,
    };
    int i;

    if (!is_power_of_2(d->iospace_size) || d->iospace_size < (1ull << 28)) {
        error_setg(errp, "virtio-dxg: iospace must be a power of two >= 256M");
        return;
    }
    config.adapter_luid_low = d->luid_low;
    config.adapter_luid_high = d->luid_high;
    config.iospace_size = d->iospace_size;
    config.linux_umd_name = d->umd_name;
    {
        /* Windows GUID byte order, as the guest's guid_to_luid() expects. */
        QemuUUID guid = qemu_uuid_bswap(d->adapter_uuid);

        memcpy(config.adapter_guid, &guid, sizeof(config.adapter_guid));
    }
    if (dxghost_create(&config, &vmm, &d->host)) {
        error_setg(errp, "virtio-dxg: no usable host GPU (D3DKMT)");
        return;
    }

    d->state = g_new0(VirtIODxgState, 1);
    QTAILQ_INIT(&d->state->mappings);
    QSIMPLEQ_INIT(&d->state->events);
    qemu_mutex_init(&d->state->event_lock);
    d->state->event_bh = qemu_bh_new(dxg_flush_events, d);
    memory_region_init(&d->iospace, OBJECT(d), "virtio-dxg-iospace",
                       d->iospace_size);

    virtio_init(vdev, VIRTIO_ID_DXG, sizeof(struct virtio_dxg_config));
    d->ctrl_vq = virtio_add_queue(vdev, DXG_QUEUE_SIZE, dxg_handle_ctrl);
    d->event_vq = virtio_add_queue(vdev, DXG_QUEUE_SIZE, dxg_handle_event);

    for (i = 0; i < DXG_LANES; i++) {
        DxgLane *lane = &d->state->lanes[i];

        lane->dev = d;
        qemu_mutex_init(&lane->lock);
        qemu_cond_init(&lane->cond);
        QSIMPLEQ_INIT(&lane->queue);
        qemu_thread_create(&lane->thread, "virtio-dxg", dxg_lane_main, lane,
                           QEMU_THREAD_JOINABLE);
    }
}

static void dxg_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIODxg *d = VIRTIO_DXG(dev);
    int i;

    dxg_drain(d);
    for (i = 0; i < DXG_LANES; i++) {
        DxgLane *lane = &d->state->lanes[i];

        qemu_mutex_lock(&lane->lock);
        lane->quit = true;
        qemu_cond_signal(&lane->cond);
        qemu_mutex_unlock(&lane->lock);
        qemu_thread_join(&lane->thread);
        qemu_cond_destroy(&lane->cond);
        qemu_mutex_destroy(&lane->lock);
    }
    dxghost_destroy(d->host);
    qemu_bh_delete(d->state->event_bh);
    qemu_mutex_destroy(&d->state->event_lock);
    g_free(d->state);
    virtio_delete_queue(d->ctrl_vq);
    virtio_delete_queue(d->event_vq);
    virtio_cleanup(vdev);
}

static const Property dxg_properties[] = {
    DEFINE_PROP_SIZE("iospace", VirtIODxg, iospace_size, 16ull << 30),
    DEFINE_PROP_UINT32("luid-low", VirtIODxg, luid_low, 0),
    DEFINE_PROP_INT32("luid-high", VirtIODxg, luid_high, 0),
    DEFINE_PROP_STRING("umd", VirtIODxg, umd_name),
    DEFINE_PROP_UUID("adapter-uuid", VirtIODxg, adapter_uuid),
};

static void dxg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, dxg_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "Windows host GPU paravirtualization (dxgkrnl over virtio)";
    vdc->realize = dxg_realize;
    vdc->unrealize = dxg_unrealize;
    vdc->get_config = dxg_get_config;
    vdc->set_config = dxg_set_config;
    vdc->get_features = dxg_get_features;
    vdc->reset = dxg_reset;
}

static const TypeInfo dxg_info = {
    .name = TYPE_VIRTIO_DXG,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIODxg),
    .class_init = dxg_class_init,
};

/* --- PCI transport ----------------------------------------------------- */

#define TYPE_VIRTIO_DXG_PCI "virtio-dxg-pci-base"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIODxgPCI, VIRTIO_DXG_PCI)

struct VirtIODxgPCI {
    VirtIOPCIProxy parent_obj;
    VirtIODxg vdev;
};

static void dxg_pci_realize(VirtIOPCIProxy *vpci, Error **errp)
{
    VirtIODxgPCI *p = VIRTIO_DXG_PCI(vpci);
    DeviceState *vdev = DEVICE(&p->vdev);

    /* Modern only; BAR 4 carries the IO space like virtio-gpu hostmem. */
    virtio_pci_force_virtio_1(vpci);
    vpci->modern_mem_bar_idx = 2;
    vpci->msix_bar_idx = 1;
    p->vdev.pci = &vpci->pci_dev;
    if (!qdev_realize(vdev, BUS(&vpci->bus), errp)) {
        return;
    }
    pci_register_bar(&vpci->pci_dev, DXG_IOSPACE_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &p->vdev.iospace);
    virtio_pci_add_shm_cap(vpci, DXG_IOSPACE_BAR, 0, p->vdev.iospace_size,
                           VIRTIO_DXG_SHM_IOSPACE);
}

static void dxg_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    k->realize = dxg_pci_realize;
    pcidev_k->class_id = PCI_CLASS_DISPLAY_OTHER;
}

static void dxg_pci_instance_init(Object *obj)
{
    VirtIODxgPCI *p = VIRTIO_DXG_PCI(obj);

    virtio_instance_init_common(obj, &p->vdev, sizeof(p->vdev),
                                TYPE_VIRTIO_DXG);
}

static const VirtioPCIDeviceTypeInfo dxg_pci_info = {
    .base_name = TYPE_VIRTIO_DXG_PCI,
    .generic_name = "virtio-dxg-pci",
    .instance_size = sizeof(VirtIODxgPCI),
    .instance_init = dxg_pci_instance_init,
    .class_init = dxg_pci_class_init,
};

static void dxg_register_types(void)
{
    type_register_static(&dxg_info);
    virtio_pci_types_register(&dxg_pci_info);
}

type_init(dxg_register_types)
