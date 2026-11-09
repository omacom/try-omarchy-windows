/*
 * virtio-dxg: Windows host GPU paravirtualization for Linux guests.
 *
 * Carries the guest's dxgkrnl messages to dxghost, which executes them with
 * the host GPU driver through D3DKMT. GPU memory the guest touches with the
 * CPU is mapped into the IO-space BAR; commands, fences and resources never
 * pass through QEMU as data.
 *
 * Requests run on lane threads, one ordered lane per guest process (hashed),
 * so a slow call in one guest process never stalls another. Requests that
 * cannot block (signals, submissions, GPU waits, event-completed CPU waits)
 * run directly on the vCPU thread that kicked the queue when their lane is
 * idle: no thread wakeup to start them, none to complete them. Host events
 * (fence waits completing) are delivered by the thread that observed them.
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
#include "hw/display/virtio-gpu-scanout.h"
#include "qemu/timer.h"
#include "ui/console.h"
#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#endif
#include "system/memory.h"
#include "system/whpx.h"
#include "system/whpx-internal.h"
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

/*
 * A host view mapped into the IO-space BAR. Its own QOM object owns the
 * region, so the flatviews that still reference the region after it is
 * removed (until their RCU destruction) keep it alive, and QOM frees it
 * after the last one, as virtio-gpu does for blob mappings.
 */
#define TYPE_VIRTIO_DXG_VIEW "virtio-dxg-view"
OBJECT_DECLARE_SIMPLE_TYPE(DxgMapping, VIRTIO_DXG_VIEW)

struct DxgMapping {
    Object parent_obj;
    MemoryRegion mr;
    uint64_t offset;
    uint64_t size;
    QTAILQ_ENTRY(DxgMapping) next;
};

typedef struct DxgEvent {
    uint32_t size;
    QSIMPLEQ_ENTRY(DxgEvent) next;
    uint8_t packet[];
} DxgEvent;

struct VirtIODxgState {
    DxgLane lanes[DXG_LANES];
    QTAILQ_HEAD(, DxgMapping) mappings;     /* memory API views (BQL) */
    /*
     * Views go straight into the WHPX partition: the IO-space BAR is an
     * MMIO container without RAM, so its guest-physical range is unmapped
     * there and a view costs one hypervisor call. Through the memory API a
     * view cost a flatview rebuild under the BQL (3.3 ms each with a
     * desktop's views alive, stalling every vCPU meanwhile). Nothing in
     * QEMU reads these addresses: the guest maps them VM_PFNMAP, which
     * Linux never hands to devices for DMA.
     */
    bool direct;
    QemuMutex direct_lock;
    GHashTable *fallback;   /* gpa of views that went the memory API way */
    QemuMutex event_lock;
    QSIMPLEQ_HEAD(, DxgEvent) events;
    /* Reply buffer of inline requests; they run under the BQL. */
    uint8_t *inline_result;
};

/* --- dxghost callbacks ------------------------------------------------- */

/* Where the guest placed the IO-space BAR, or 0. */
static uint64_t dxg_iospace_gpa(VirtIODxg *d)
{
    pcibus_t addr = d->pci ? pci_get_bar_addr(d->pci, DXG_IOSPACE_BAR) :
                             PCI_BAR_UNMAPPED;

    return addr == PCI_BAR_UNMAPPED ? 0 : addr;
}

/* DXGHOST_STATS: where guest-physical view changes spend their time. */
static struct {
    uint64_t maps, bql_wait_us, update_us;
    uint64_t direct_unmaps, direct_unmap_failures;
} dxg_map_stats;

static int dxg_map(void *opaque, uint64_t gpa, void *hva, uint64_t size,
                   bool writable)
{
    VirtIODxg *d = opaque;
    g_autofree char *name = NULL;
    uint64_t base;
    DxgMapping *m;
    bool locked;
    int64_t t0 = g_get_monotonic_time(), t1;

    if (d->state->direct) {
        base = dxg_iospace_gpa(d);
        if (base && gpa >= base && size <= d->iospace_size &&
            gpa - base <= d->iospace_size - size) {
            HRESULT hr = whp_dispatch.WHvMapGpaRange(
                whpx_global.partition, hva, gpa, size,
                WHvMapGpaRangeFlagRead |
                (writable ? WHvMapGpaRangeFlagWrite : 0));

            if (SUCCEEDED(hr)) {
                if (d->stats) {
                    dxg_map_stats.maps++;
                    dxg_map_stats.update_us += g_get_monotonic_time() - t0;
                    if (dxg_map_stats.maps % 2000 == 0) {
                        warn_report("virtio-dxg: %" PRIu64 " views mapped "
                                    "directly: %" PRIu64 " us avg, %" PRIu64
                                    " live, %" PRIu64 " unmap failures",
                                    dxg_map_stats.maps,
                                    dxg_map_stats.update_us /
                                    dxg_map_stats.maps,
                                    dxg_map_stats.maps -
                                    dxg_map_stats.direct_unmaps,
                                    dxg_map_stats.direct_unmap_failures);
                    }
                }
                return 0;
            }
            warn_report_once("virtio-dxg: direct view mapping failed "
                             "(0x%08lx); using the memory API",
                             (unsigned long)hr);
        }
        qemu_mutex_lock(&d->state->direct_lock);
        g_hash_table_add(d->state->fallback, g_memdup2(&gpa, sizeof(gpa)));
        qemu_mutex_unlock(&d->state->direct_lock);
    }
    locked = bql_locked();
    if (!locked) {
        bql_lock();
    }
    t1 = g_get_monotonic_time();
    base = dxg_iospace_gpa(d);
    if (!base || gpa < base || size > d->iospace_size ||
        gpa - base > d->iospace_size - size) {
        if (!locked) {
            bql_unlock();
        }
        return -1;
    }
    m = VIRTIO_DXG_VIEW(object_new(TYPE_VIRTIO_DXG_VIEW));
    m->offset = gpa - base;
    m->size = size;
    name = g_strdup_printf("view-%" PRIx64, m->offset);
    object_property_add_child(OBJECT(d), name, OBJECT(m));
    object_unref(OBJECT(m));            /* the device's child reference */
    memory_region_init_ram_ptr(&m->mr, OBJECT(m), "mr", size, hva);
    memory_region_set_readonly(&m->mr, !writable);
    memory_region_add_subregion(&d->iospace, m->offset, &m->mr);
    QTAILQ_INSERT_TAIL(&d->state->mappings, m, next);
    if (d->stats) {
        dxg_map_stats.maps++;
        dxg_map_stats.bql_wait_us += t1 - t0;
        dxg_map_stats.update_us += g_get_monotonic_time() - t1;
        if (dxg_map_stats.maps % 2000 == 0) {
            warn_report("virtio-dxg: %" PRIu64 " views mapped: BQL wait %"
                        PRIu64 " us, memory map update %" PRIu64 " us avg",
                        dxg_map_stats.maps,
                        dxg_map_stats.bql_wait_us / dxg_map_stats.maps,
                        dxg_map_stats.update_us / dxg_map_stats.maps);
        }
    }
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
    bool locked;

    if (d->state->direct) {
        bool fallback;

        qemu_mutex_lock(&d->state->direct_lock);
        fallback = g_hash_table_remove(d->state->fallback, &gpa);
        qemu_mutex_unlock(&d->state->direct_lock);
        if (!fallback) {
            HRESULT hr = whp_dispatch.WHvUnmapGpaRange(whpx_global.partition,
                                                       gpa, size);

            dxg_map_stats.direct_unmaps++;
            if (FAILED(hr)) {
                dxg_map_stats.direct_unmap_failures++;
                warn_report_once("virtio-dxg: unmapping a view failed "
                                 "(0x%08lx)", (unsigned long)hr);
            }
            return;
        }
    }
    locked = bql_locked();
    if (!locked) {
        bql_lock();
    }
    offset = gpa - dxg_iospace_gpa(d);
    QTAILQ_FOREACH(m, &d->state->mappings, next) {
        if (m->offset == offset && m->size == size) {
            QTAILQ_REMOVE(&d->state->mappings, m, next);
            memory_region_del_subregion(&d->iospace, &m->mr);
            object_unparent(OBJECT(m));   /* freed after the last flatview */
            break;
        }
    }
    if (!locked) {
        bql_unlock();
    }
}

/*
 * One memory transaction for a batch of unmaps: the IO space's flatview is
 * rebuilt once, and the BQL held once, instead of per view.
 */
static __thread bool dxg_batch_took_bql;

static void dxg_batch(void *opaque, bool begin)
{
    VirtIODxg *d = opaque;

    if (d->state->direct) {
        return;     /* direct views: no memory map to batch */
    }
    if (begin) {
        dxg_batch_took_bql = !bql_locked();
        if (dxg_batch_took_bql) {
            bql_lock();
        }
        memory_region_transaction_begin();
    } else {
        memory_region_transaction_commit();
        if (dxg_batch_took_bql) {
            bql_unlock();
        }
        dxg_batch_took_bql = false;
    }
}

static void dxg_flush_events(VirtIODxg *d);

/*
 * Delivered right away by the calling thread (a host thread-pool thread
 * when a fence wait completes), not through the main loop: one thread
 * wakeup less between the GPU signalling a fence and the guest waking up.
 */
static void dxg_event(void *opaque, const void *packet, uint32_t size)
{
    VirtIODxg *d = opaque;
    DxgEvent *e = g_malloc(sizeof(*e) + size);

    e->size = size;
    memcpy(e->packet, packet, size);
    qemu_mutex_lock(&d->state->event_lock);
    QSIMPLEQ_INSERT_TAIL(&d->state->events, e, next);
    qemu_mutex_unlock(&d->state->event_lock);
    if (bql_locked()) {
        dxg_flush_events(d);
    } else {
        bql_lock();
        dxg_flush_events(d);
        bql_unlock();
    }
}

static void dxg_log(void *opaque, const char *message)
{
    (void)opaque;
    warn_report("virtio-dxg: %s", message);
}

/* Moves queued host events into guest buffers. BQL held. */
static void dxg_flush_events(VirtIODxg *d)
{
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

/* Returns a request to the guest; BQL held. The caller notifies. */
static void dxg_finish(VirtIODxg *d, DxgRequest *r, uint8_t *result,
                       const struct virtio_dxg_resp *resp)
{
    size_t written = 0;

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
}

static void dxg_complete(VirtIODxg *d, DxgRequest *r, uint8_t *result,
                         const struct virtio_dxg_resp *resp)
{
    bql_lock();
    dxg_finish(d, r, result, resp);
    virtio_notify(VIRTIO_DEVICE(d), d->ctrl_vq);
    bql_unlock();
}

/*
 * Windows runs the threads of processes it considers background (no
 * foreground window, or flagged efficient) at low QoS: on efficiency cores at
 * low clocks, with their timer requests ignored while the window is hidden.
 * Then a vCPU or a GPU request lane wakes in milliseconds where it took
 * microseconds, and the guest's frames and calls stall. QEMU opts out for
 * the whole process (execution speed and timer resolution), and the lanes,
 * which carry the guest's GPU calls, also run above normal priority.
 * Information classes: ProcessPowerThrottling 4, ThreadPowerThrottling 3.
 */
typedef BOOL (WINAPI *DxgSetInfo)(HANDLE, int, LPVOID, DWORD);

typedef struct DxgPowerThrottling {
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
} DxgPowerThrottling;

static void dxg_high_qos_process(void)
{
    DxgSetInfo set = (DxgSetInfo)(void (*)(void))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    /* Controlled, and off: never throttled, timer requests always honored. */
    DxgPowerThrottling s = { 1, 0x1 | 0x4, 0 };

    if (!set || !set(GetCurrentProcess(), 4, &s, sizeof(s))) {
        warn_report("virtio-dxg: cannot opt out of power throttling: %lu",
                    (unsigned long)GetLastError());
    }
}

static void dxg_high_qos_thread(void)
{
    DxgSetInfo set = (DxgSetInfo)(void (*)(void))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation");
    DxgPowerThrottling s = { 1, 0x1, 0 };

    if (set) {
        set(GetCurrentThread(), 3, &s, sizeof(s));
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

static void *dxg_lane_main(void *opaque)
{
    DxgLane *lane = opaque;
    VirtIODxg *d = lane->dev;
    g_autofree uint8_t *result = g_malloc(VIRTIO_DXG_MAX_MESSAGE);

    dxg_high_qos_thread();

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

/* --- Zero-copy scanout -------------------------------------------------- */

/*
 * The guest's compositor renders into shared D3D12 textures, each paired
 * with a virtio-gpu 2D scanout buffer. A link opens the texture here (an NT
 * handle from dxghost); virtio-gpu then neither copies the buffer's guest
 * memory nor uploads a surface, and the display (SDL's OpenGL window, on the
 * same GPU) draws the texture itself: GL_EXT_memory_object_win32 imports it
 * as a GL texture, and presenting a frame is one GPU blit. Links, textures
 * and statistics are all touched under the BQL: links come from the control
 * queue handler, the hooks from virtio-gpu's command processing.
 */
#ifdef CONFIG_OPENGL

#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

typedef struct DxgScanout {
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t alloc_size;
    uint64_t cookie;
    HANDLE nt;
    GLuint mem;
    GLuint tex;
    bool failed;
} DxgScanout;

static struct {
    GHashTable *links;          /* resource id -> DxgScanout */
    GArray *garbage;            /* GL names to delete with the context */
    QemuConsole *con;           /* console whose context imports */
    QEMUGLContext ctx;
    QEMUTimer *probe;           /* until the display exists */
    int probes;
    bool ready;                 /* the display can import D3D12 textures */
    bool stats;
    GLuint shown_tex;           /* what the console's scanout shows */
    /* Presentation: frames, and gaps between them over 1.5 periods. */
    int64_t last_ns;
    int64_t window_ns;
    uint64_t frames;
    uint64_t late;
    int64_t worst_ns;
    int64_t present_ns;         /* dpy_gl_update: blit and swap */
    int64_t present_worst_ns;
} dxg_scanout;

static void dxg_scanout_free(gpointer data)
{
    DxgScanout *s = data;

    /* GL objects go with the context current (main loop); see collect. */
    if (s->tex || s->mem) {
        GLuint names[2] = { s->tex, s->mem };

        g_array_append_vals(dxg_scanout.garbage, names, 2);
    }
    if (s->nt) {
        CloseHandle(s->nt);
    }
    g_free(s);
}

static void dxg_scanout_collect(void)
{
    guint i;

    for (i = 0; i < dxg_scanout.garbage->len; i += 2) {
        GLuint tex = g_array_index(dxg_scanout.garbage, GLuint, i);
        GLuint mem = g_array_index(dxg_scanout.garbage, GLuint, i + 1);

        if (tex) {
            if (tex == dxg_scanout.shown_tex) {
                dxg_scanout.shown_tex = 0;
            }
            glDeleteTextures(1, &tex);
        }
        if (mem) {
            glDeleteMemoryObjectsEXT(1, &mem);
        }
    }
    g_array_set_size(dxg_scanout.garbage, 0);
}

/* GL internal format for a DRM fourcc (memory order), or 0. */
static GLenum dxg_scanout_format(uint32_t fourcc)
{
    switch (fourcc) {
    case 0x34325258:    /* XR24: XRGB8888, B G R X in memory */
    case 0x34325241:    /* AR24 */
        return GL_BGRA8_EXT;
    case 0x34324258:    /* XB24: XBGR8888, R G B X in memory */
    case 0x34324241:    /* AB24 */
        return GL_RGBA8;
    default:
        return 0;
    }
}

/*
 * Main loop: can the display's GL import D3D12 textures? QEMU runs the main
 * loop during start-up (block devices poll it) before the display exists,
 * so this tries again every 200 ms for 10 s. Links made before it answers
 * keep the guest's copies.
 */
static void dxg_scanout_probe(void *opaque)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    QEMUGLParams params = { .major_ver = 4, .minor_ver = 5 };

    if (!con || !console_has_gl(con)) {
        if (++dxg_scanout.probes < 50) {
            timer_mod(dxg_scanout.probe,
                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 200);
            return;
        }
        info_report("virtio-dxg: zero-copy scanout off (the display has no "
                    "OpenGL; -display sdl,gl=on enables it)");
        return;
    }
    dxg_scanout.ctx = dpy_gl_ctx_create(con, &params);
    if (!dxg_scanout.ctx) {
        warn_report("virtio-dxg: zero-copy scanout off: no GL context");
        return;
    }
    dpy_gl_ctx_make_current(con, dxg_scanout.ctx);
    if (!epoxy_has_gl_extension("GL_EXT_memory_object_win32")) {
        warn_report("virtio-dxg: zero-copy scanout off: %s lacks "
                    "GL_EXT_memory_object_win32", glGetString(GL_RENDERER));
        return;
    }
    dxg_scanout.con = con;
    dxg_scanout.ready = true;
    info_report("virtio-dxg: zero-copy scanout on %s, %u.%03u Hz",
                glGetString(GL_RENDERER),
                (qemu_edid_refresh_rate_mhz ?: 120000) / 1000,
                (qemu_edid_refresh_rate_mhz ?: 120000) % 1000);
}

static bool dxg_scanout_link(VirtIODxg *d,
                             const struct virtio_dxg_device_cmd *c)
{
    uint32_t id = le32_to_cpu(c->gpu_resource);
    uint64_t host = le64_to_cpu(c->host_resource);
    uint64_t cookie = le64_to_cpu(c->cookie);
    DxgScanout *s;
    void *nt = NULL;
    int32_t status;

    if (!host) {
        /* Unlink, unless a later link of the buffer replaced this one. */
        s = g_hash_table_lookup(dxg_scanout.links, GUINT_TO_POINTER(id));
        if (dxg_scanout.stats) {
            info_report("virtio-dxg: scanout %u unlinked%s", id,
                        s && s->cookie == cookie ? "" : " (stale)");
        }
        if (s && s->cookie == cookie) {
            g_hash_table_remove(dxg_scanout.links, GUINT_TO_POINTER(id));
        }
        return false;
    }
    /*
     * Cursor-sized buffers stay in guest memory: virtio-gpu's cursor reads
     * them there.
     */
    if (!dxg_scanout.ready || !id ||
        !dxg_scanout_format(le32_to_cpu(c->drm_format)) ||
        (le32_to_cpu(c->width) <= 256 && le32_to_cpu(c->height) <= 256) ||
        le32_to_cpu(c->width) > 16384 || le32_to_cpu(c->height) > 16384) {
        return false;
    }
    status = dxghost_share_resource(d->host, (uint32_t)host, &nt);
    if (status < 0 || !nt) {
        warn_report_once("virtio-dxg: cannot share scanout resource 0x%x: "
                         "0x%08x", (uint32_t)host, (uint32_t)status);
        return false;
    }
    s = g_new0(DxgScanout, 1);
    s->resource_id = id;
    s->width = le32_to_cpu(c->width);
    s->height = le32_to_cpu(c->height);
    s->drm_format = le32_to_cpu(c->drm_format);
    s->alloc_size = le32_to_cpu(c->alloc_size);
    s->cookie = cookie;
    s->nt = nt;
    g_hash_table_replace(dxg_scanout.links, GUINT_TO_POINTER(id), s);
    if (dxg_scanout.stats) {
        info_report("virtio-dxg: scanout %u linked to 0x%x (%ux%u %.4s)",
                    id, (uint32_t)host, s->width, s->height,
                    (const char *)&s->drm_format);
    }
    return true;
}

/* With dxg_scanout.ctx current. */
static bool dxg_scanout_import(DxgScanout *s)
{
    GLint dedicated = GL_TRUE;
    GLenum err;

    if (s->failed) {
        return false;
    }
    while (glGetError() != GL_NO_ERROR) {
        /* earlier errors are not ours */
    }
    glCreateMemoryObjectsEXT(1, &s->mem);
    glMemoryObjectParameterivEXT(s->mem, GL_DEDICATED_MEMORY_OBJECT_EXT,
                                 &dedicated);
    glImportMemoryWin32HandleEXT(s->mem, s->alloc_size,
                                 GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, s->nt);
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, dxg_scanout_format(s->drm_format),
                         s->width, s->height, s->mem, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    err = glGetError();
    if (err != GL_NO_ERROR) {
        warn_report("virtio-dxg: importing scanout %u (%ux%u, %u bytes) "
                    "failed: GL error 0x%x", s->resource_id, s->width,
                    s->height, s->alloc_size, err);
        s->failed = true;
        return false;
    }
    return true;
}

static bool dxg_scanout_linked(uint32_t resource_id)
{
    DxgScanout *s = dxg_scanout.links ?
        g_hash_table_lookup(dxg_scanout.links,
                            GUINT_TO_POINTER(resource_id)) : NULL;

    return s && !s->failed;
}

static bool dxg_scanout_show(QemuConsole *con, uint32_t resource_id,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    DxgScanout *s;

    if (!dxg_scanout.ready || con != dxg_scanout.con) {
        return false;
    }
    s = g_hash_table_lookup(dxg_scanout.links, GUINT_TO_POINTER(resource_id));
    if (!s || s->failed) {
        if (dxg_scanout.stats && g_hash_table_size(dxg_scanout.links)) {
            warn_report_once("virtio-dxg: scanout of unlinked resource %u "
                             "(%u linked)", resource_id,
                             g_hash_table_size(dxg_scanout.links));
        }
        return false;
    }
    dpy_gl_ctx_make_current(con, dxg_scanout.ctx);
    dxg_scanout_collect();
    if (!s->tex && !dxg_scanout_import(s)) {
        return false;
    }
    /*
     * Row 0 of a D3D12 texture is the top of the image, and GL sees it as
     * row 0 too: the display flips it to the window's bottom-up origin,
     * as it does a GL-rendered (bottom-up) scanout. So, for the display,
     * not "y 0 top".
     */
    dpy_gl_scanout_texture(con, s->tex, false, s->width, s->height,
                           x, y, w, h, NULL);
    dxg_scanout.shown_tex = s->tex;
    return true;
}

static void dxg_scanout_count(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint32_t mhz = qemu_edid_refresh_rate_mhz ?: 120000;
    int64_t period = 1000000000000ll / mhz;

    if (dxg_scanout.last_ns) {
        int64_t gap = now - dxg_scanout.last_ns;

        /* Gaps over 100 ms are an idle screen, not a dropped frame. */
        if (gap < 100 * SCALE_MS) {
            if (gap > period * 3 / 2) {
                dxg_scanout.late++;
            }
            dxg_scanout.worst_ns = MAX(dxg_scanout.worst_ns, gap);
        }
    } else {
        dxg_scanout.window_ns = now;
    }
    dxg_scanout.last_ns = now;
    dxg_scanout.frames++;
    if (dxg_scanout.stats && now - dxg_scanout.window_ns >= 10 * NANOSECONDS_PER_SECOND) {
        double secs = (now - dxg_scanout.window_ns) / 1e9;

        info_report("virtio-dxg: scanout %.1f fps over %.0f s (display "
                    "%.0f Hz): %" PRIu64 " gaps over 1.5 frames, worst "
                    "%.2f ms; present %.3f ms, worst %.2f ms",
                    dxg_scanout.frames / secs, secs, mhz / 1000.0,
                    dxg_scanout.late, dxg_scanout.worst_ns / 1e6,
                    dxg_scanout.present_ns / 1e6 / dxg_scanout.frames,
                    dxg_scanout.present_worst_ns / 1e6);
        dxg_scanout.window_ns = now;
        dxg_scanout.frames = 0;
        dxg_scanout.late = 0;
        dxg_scanout.worst_ns = 0;
        dxg_scanout.present_ns = 0;
        dxg_scanout.present_worst_ns = 0;
    }
}

static void dxg_scanout_flush(QemuConsole *con, uint32_t resource_id,
                              uint32_t width, uint32_t height)
{
    DxgScanout *s = g_hash_table_lookup(dxg_scanout.links,
                                        GUINT_TO_POINTER(resource_id));

    /* Relinked since it was shown (a new texture for the same buffer). */
    if (s && !s->failed && s->tex != dxg_scanout.shown_tex) {
        dxg_scanout_show(con, resource_id, 0, 0, width, height);
    }
    {
        int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME), t;

        dpy_gl_update(con, 0, 0, width, height);
        t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
        dxg_scanout.present_ns += t;
        dxg_scanout.present_worst_ns = MAX(dxg_scanout.present_worst_ns, t);
    }
    dxg_scanout_count();
}

static void dxg_scanout_forget(uint32_t resource_id)
{
    g_hash_table_remove(dxg_scanout.links, GUINT_TO_POINTER(resource_id));
}

static const VirtIOGPUScanoutHooks dxg_scanout_hooks = {
    .linked = dxg_scanout_linked,
    .show = dxg_scanout_show,
    .flush = dxg_scanout_flush,
    .forget = dxg_scanout_forget,
};

static void dxg_scanout_init(VirtIODxg *d)
{
    dxg_scanout.links = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, dxg_scanout_free);
    dxg_scanout.garbage = g_array_new(false, false, sizeof(GLuint));
    dxg_scanout.stats = d->stats;
    virtio_gpu_scanout_hooks = &dxg_scanout_hooks;
    dxg_scanout.probe = timer_new_ms(QEMU_CLOCK_REALTIME, dxg_scanout_probe,
                                     d);
    timer_mod(dxg_scanout.probe, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 200);
}

static void dxg_scanout_reset(void)
{
    if (dxg_scanout.links) {
        g_hash_table_remove_all(dxg_scanout.links);
    }
}

static void dxg_scanout_fini(void)
{
    virtio_gpu_scanout_hooks = NULL;
    if (dxg_scanout.probe) {
        timer_free(dxg_scanout.probe);
        dxg_scanout.probe = NULL;
    }
    dxg_scanout_reset();
}

#else /* !CONFIG_OPENGL */

static bool dxg_scanout_link(VirtIODxg *d,
                             const struct virtio_dxg_device_cmd *c)
{
    return false;
}

static void dxg_scanout_init(VirtIODxg *d)
{
}

static void dxg_scanout_reset(void)
{
}

static void dxg_scanout_fini(void)
{
}

#endif /* CONFIG_OPENGL */

/* --- The guest's refresh rate follows the host monitor ------------------ */

/*
 * The display gives the guest the window's size; its refresh rate SDL reads
 * once, from the primary display, at start. Once a second (main loop, the
 * window's own thread) this looks at the monitor showing the window and,
 * when its rate differs, gives the guest the new one with the window's size
 * (dpy_set_ui_info: virtio-gpu's EDID). A 240 Hz panel thus gives a 240 Hz
 * guest mode, and moving the window to a 60 Hz monitor a 60 Hz one.
 */
static QEMUTimer *dxg_refresh_timer;

static BOOL CALLBACK dxg_find_window(HWND hwnd, LPARAM out)
{
    char cls[16];

    if (IsWindowVisible(hwnd) && GetClassNameA(hwnd, cls, sizeof(cls)) &&
        !strcmp(cls, "SDL_app")) {
        *(HWND *)out = hwnd;
        return FALSE;
    }
    return TRUE;
}

static void dxg_follow_refresh(void *opaque)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    const QemuUIInfo *cur;
    HWND hwnd = NULL;
    uint32_t mhz;

    timer_mod(dxg_refresh_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    if (!con) {
        return;
    }
    EnumThreadWindows(GetCurrentThreadId(), dxg_find_window, (LPARAM)&hwnd);
    if (!hwnd ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                         (MONITORINFO *)&mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ||
        dm.dmDisplayFrequency < 24) {
        return;
    }
    mhz = dm.dmDisplayFrequency * 1000;
    cur = dpy_get_ui_info(con);
    if (mhz == (cur->refresh_rate ?: qemu_edid_refresh_rate_mhz)) {
        return;
    }
    /* Also SDL's input polling, which runs once per display frame. */
    qemu_edid_refresh_rate_mhz = mhz;
    if (cur->width && cur->height) {
        QemuUIInfo info = *cur;

        info.refresh_rate = mhz;
        dpy_set_ui_info(con, &info, false);
    }
    info_report("virtio-dxg: the window's display runs at %u Hz",
                (unsigned)dm.dmDisplayFrequency);
}

/* VIRTIO_DXG_CHANNEL_DEVICE: the device's own commands. BQL. */
static void dxg_device_command(VirtIODxg *d, const uint8_t *message,
                               uint32_t size, uint8_t *result,
                               uint32_t result_size,
                               struct virtio_dxg_resp *resp)
{
    struct virtio_dxg_device_cmd c;
    struct virtio_dxg_device_reply reply = { 0 };

    memset(resp, 0, sizeof(*resp));
    if (size < sizeof(c) || result_size < sizeof(reply)) {
        resp->status = VIRTIO_DXG_ERR_INVALID;
        return;
    }
    memcpy(&c, message, sizeof(c));
    switch (le32_to_cpu(c.command)) {
    case VIRTIO_DXG_DEV_SCANOUT_LINK:
        if (dxg_scanout_link(d, &c)) {
            reply.flags = cpu_to_le32(VIRTIO_DXG_SCANOUT_ZERO_COPY);
        }
        break;
    default:
        resp->status = VIRTIO_DXG_ERR_UNSUPPORTED;
        return;
    }
    memcpy(result, &reply, sizeof(reply));
    resp->size = sizeof(reply);
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
    bool notify = false;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        struct virtio_dxg_req hdr;
        DxgRequest *r;
        DxgLane *lane;
        struct virtio_dxg_resp resp;
        size_t in_size = iov_size(elem->in_sg, elem->in_num);

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr)) !=
            sizeof(hdr)) {
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            notify = true;
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
            warn_report("virtio-dxg: rejected request: %u bytes, result %u, "
                        "reply room %zu", r->size, r->result_size, in_size);
            g_free(r);
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            notify = true;
            continue;
        }
        r->message = g_malloc(r->size);
        if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), r->message,
                       r->size) != r->size) {
            warn_report("virtio-dxg: short request: %u bytes announced, %zu "
                        "sent", r->size,
                        iov_size(elem->out_sg, elem->out_num) - sizeof(hdr));
            g_free(r->message);
            g_free(r);
            dxg_fail(d, elem, VIRTIO_DXG_ERR_INVALID);
            notify = true;
            continue;
        }
        if (r->channel == VIRTIO_DXG_CHANNEL_DEVICE) {
            dxg_device_command(d, r->message, r->size,
                               d->state->inline_result, r->result_size,
                               &resp);
            dxg_finish(d, r, d->state->inline_result, &resp);
            g_free(r->elem);
            g_free(r->message);
            g_free(r);
            notify = true;
            continue;
        }
        lane = &d->state->lanes[dxghost_lane(r->message, r->size) %
                                DXG_LANES];
        qemu_mutex_lock(&lane->lock);
        /* The lane lock keeps this lane's next requests behind it. */
        if (!lane->busy && QSIMPLEQ_EMPTY(&lane->queue) &&
            dxghost_try_inline(d->host, r->channel, r->message, r->size,
                               r->async ? NULL : d->state->inline_result,
                               r->result_size, &resp)) {
            qemu_mutex_unlock(&lane->lock);
            dxg_finish(d, r, d->state->inline_result, &resp);
            g_free(r->elem);
            g_free(r->message);
            g_free(r);
            notify = true;
            continue;
        }
        QSIMPLEQ_INSERT_TAIL(&lane->queue, r, next);
        lane->busy = true;
        qemu_cond_signal(&lane->cond);
        qemu_mutex_unlock(&lane->lock);
    }
    if (notify) {
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
    /*
     * Without the BQL: resetting waits for in-flight host event callbacks,
     * which take it to deliver, and unmaps views through dxg_unmap, which
     * takes it itself.
     */
    bql_unlock();
    dxghost_reset(d->host);
    bql_lock();
    dxg_scanout_reset();
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
        .batch = dxg_batch,
        .log = dxg_log,
    };
    int i;

    if (!is_power_of_2(d->iospace_size) || d->iospace_size < (1ull << 28)) {
        error_setg(errp, "virtio-dxg: iospace must be a power of two >= 256M");
        return;
    }
    d->stats = getenv("DXGHOST_STATS") != NULL;
    if (!getenv("VIRTIO_DXG_DEFAULT_QOS")) {
        dxg_high_qos_process();
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
    d->state->direct = whpx_enabled() && !getenv("VIRTIO_DXG_MEMORY_API");
    qemu_mutex_init(&d->state->direct_lock);
    d->state->fallback = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free, NULL);
    QSIMPLEQ_INIT(&d->state->events);
    qemu_mutex_init(&d->state->event_lock);
    d->state->inline_result = g_malloc(VIRTIO_DXG_MAX_MESSAGE);
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
    dxg_scanout_init(d);
    dxg_refresh_timer = timer_new_ms(QEMU_CLOCK_REALTIME, dxg_follow_refresh,
                                     d);
    timer_mod(dxg_refresh_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}

static void dxg_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIODxg *d = VIRTIO_DXG(dev);
    int i;

    dxg_drain(d);
    dxg_scanout_fini();
    if (dxg_refresh_timer) {
        timer_free(dxg_refresh_timer);
        dxg_refresh_timer = NULL;
    }
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
    bql_unlock();  /* as in dxg_reset */
    dxghost_destroy(d->host);
    bql_lock();
    g_free(d->state->inline_result);
    qemu_mutex_destroy(&d->state->event_lock);
    g_hash_table_destroy(d->state->fallback);
    qemu_mutex_destroy(&d->state->direct_lock);
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

static const TypeInfo dxg_view_info = {
    .name = TYPE_VIRTIO_DXG_VIEW,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(DxgMapping),
};

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

/*
 * MSI-X, one vector each for configuration, completions and events. Left
 * unset, virtio-pci gives a device no vectors and the guest falls back to a
 * shared INTx line: an ISR read and an IOAPIC EOI exit per completion, and
 * on WHPX completions that never reached the guest.
 */
static const Property dxg_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 3),
};

static void dxg_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    device_class_set_props(dc, dxg_pci_properties);
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
    type_register_static(&dxg_view_info);
    type_register_static(&dxg_info);
    virtio_pci_types_register(&dxg_pci_info);
}

type_init(dxg_register_types)
