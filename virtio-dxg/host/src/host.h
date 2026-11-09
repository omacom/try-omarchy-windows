/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DXGHOST_HOST_H
#define DXGHOST_HOST_H

#include <windows.h>
#include "wire.h"
#include "kmt.h"
#include "../include/dxghost.h"

/*
 * Every handle the guest may name is recorded with its type and the guest
 * process that created it. The QEMU process holds D3DKMT objects of its own
 * (the SDL renderer's), so an unrecorded handle is always refused.
 */
enum dxghost_obj_type {
	OBJ_NONE,
	OBJ_DEVICE,
	OBJ_CONTEXT,
	OBJ_PAGING_QUEUE,
	OBJ_ALLOCATION,
	OBJ_RESOURCE,
	OBJ_SYNC,
	OBJ_HWQUEUE,
};

struct dxghost_notify;

struct dxghost_obj {
	uint32_t handle;
	uint8_t type;
	uint8_t state;		/* 0 empty, 1 live, 2 deleted */
	uint8_t cached;		/* Lock2 view is write-back cacheable */
	uint8_t lock_cached;	/* holds one driver lock past the guest's */
	uint32_t process;	/* guest process token */
	uint32_t parent;	/* device; context for hwqueues; queue for fences */
	uint32_t resource;	/* allocations created with a resource */
	/* Lock2 mapping (allocation) or mapped fence page (sync, queues). */
	uint32_t guest_locks;	/* Lock2 calls the guest has not undone */
	void *hva;
	uint64_t gpa;
	uint64_t size;
	/* CPU notification sync objects: the host event forwarding signals. */
	struct dxghost_notify *notify;
	/* Existing-sysmem allocations: the host memory backing them. */
	void *sysmem;
};

struct dxghost_objtab {
	struct dxghost_obj *slots;
	uint32_t capacity;	/* power of two */
	uint32_t used;		/* live + deleted */
	uint32_t live;
};

struct dxghost_range {
	uint64_t start;
	uint64_t size;
};

/* A host fence page mapped into the guest, shared by every fence on it. */
struct dxghost_fence_page {
	void *hva;
	uint64_t gpa;
	uint32_t refs;
};

#define DXGHOST_STAT_TYPES 128

/* Work found under h->lock that must run after it (see struct dxghost). */
struct dxghost_deferred {
	void (*fn)(void *arg);	/* NULL: unmap gpa/size, then free it */
	void *arg;
	uint64_t gpa, size;
};

/* GPU VA range a guest process reserved; guest VA operations stay inside. */
struct dxghost_va {
	uint32_t process;
	/* 0: an explicit ReserveGpuVirtualAddress, the process must free it.
	 * Else the allocation whose base-less map created the range; the
	 * driver frees it with the allocation. */
	uint32_t allocation;
	uint64_t base;
	uint64_t size;
};

/* An armed D3DKMTWaitForSynchronizationObjectFromCpu. */
struct dxghost_wait {
	struct dxghost_wait *next;
	struct dxghost *h;
	HANDLE event;
	PTP_WAIT tp;
	uint32_t process;
	uint64_t guest_event;
	volatile LONG fired;
};

struct dxghost_proc {
	uint32_t token;
	uint64_t guest_pid;
	bool live;
};

struct dxghost {
	struct dxghost_kmt kmt;
	struct dxghost_vmm vmm;
	struct dxghost_config config;
	/*
	 * Lock order. The VMM may hold its own lock (QEMU's BQL) when it calls
	 * in: requests run inline on the vCPU thread that kicked the queue.
	 * So while h->lock is held nothing may call into the VMM (map, unmap,
	 * event) or wait for a callback that does; such work is deferred and
	 * runs in dxghost_unlock() after the lock is released, before the
	 * request's reply. map_lock serializes guest-physical mapping changes
	 * outside h->lock; requests that run inline never take it.
	 */
	SRWLOCK lock;		/* tables, IO space, adapter state */
	SRWLOCK map_lock;
	/* Bytes of Lock2 views kept mapped (and locked) between guest locks. */
	uint64_t lock_cache_bytes;
	uint64_t lock_cache_max;	/* DXGHOST_LOCK_CACHE_MB, default 1024 */
	struct dxghost_deferred *deferred;
	uint32_t deferred_count, deferred_capacity;

	LUID luid;
	uint32_t adapter;	/* opened at OPENADAPTER */
	uint32_t adapter_refs;
	wchar_t description[128];

	uint64_t io_base;
	uint64_t io_size;
	struct dxghost_range *io_free;
	uint32_t io_free_count;
	uint32_t io_free_capacity;

	struct dxghost_fence_page *fence_pages;
	uint32_t fence_page_count;
	uint32_t fence_page_capacity;

	struct dxghost_objtab objects;

	struct dxghost_proc *procs;
	uint32_t proc_count;
	uint32_t proc_capacity;
	uint32_t next_proc;

	struct dxghost_va *vas;
	uint32_t va_count;
	uint32_t va_capacity;

	bool trace;
	/*
	 * DXGHOST_STATS: calls and host time per command type, written to the
	 * log every 10 s ([0] global, [1] adapter commands).
	 */
	bool stats;
	bool inline_lock2, inline_unlock2;
	struct dxghost_stat {
		volatile LONG64 count, ticks, max;
	} stat[2][DXGHOST_STAT_TYPES];
	volatile LONG64 stats_due;
	LONG64 qpf;
	/* Existing-sysmem backing freed once the driver has let go (lock). */
	void **sysmem_free;
	uint32_t sysmem_free_count, sysmem_free_capacity;
	SRWLOCK wait_lock;
	struct dxghost_wait *waits;
	/* Completed waits kept for reuse: no event or thread-pool object is
	 * created per CPU wait once the pool is warm. */
	struct dxghost_wait *wait_pool;
	volatile LONG closing;
};

/* objects.c */
int dxghost_obj_add(struct dxghost *h, uint32_t handle, uint8_t type,
		    uint32_t process, uint32_t parent);
struct dxghost_obj *dxghost_obj_find(struct dxghost *h, uint32_t handle);
struct dxghost_obj *dxghost_obj_get(struct dxghost *h, uint32_t handle,
				    uint8_t type, uint32_t process);
void dxghost_obj_remove(struct dxghost *h, uint32_t handle);
void dxghost_objtab_free(struct dxghost_objtab *t);

/* CloseHandle() of an NT handle from dxghost_share_resource(); NULL is ok. */
void dxghost_close_nt(void *nt_handle);

/* h->lock: dxghost_lock(); dxghost_unlock() runs the deferred work. */
void dxghost_lock(struct dxghost *h);
void dxghost_unlock(struct dxghost *h);
/* With h->lock held: unmap a guest-physical range and free its IO space
 * after the lock is released; call fn(arg) then. */
void dxghost_defer_unmap(struct dxghost *h, uint64_t gpa, uint64_t size);
void dxghost_defer_call(struct dxghost *h, void (*fn)(void *), void *arg);

void dxghost_io_reset(struct dxghost *h, uint64_t base, uint64_t size);
uint64_t dxghost_io_alloc(struct dxghost *h, uint64_t size);
void dxghost_io_free(struct dxghost *h, uint64_t gpa, uint64_t size);

/*
 * Maps the page holding a fence at hva into the IO space and returns the
 * fence's exact guest-physical address (page + offset), or 0. Call without
 * h->lock. dxghost_fence_unmap() drops a reference, with h->lock held.
 */
uint64_t dxghost_fence_map(struct dxghost *h, void *fence_hva);
void dxghost_fence_unmap(struct dxghost *h, uint64_t fence_gpa);

/* dxghost.c */
#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void dxghost_logf(struct dxghost *h, const char *format, ...);
void dxghost_signal_guest_event(struct dxghost *h, uint32_t process,
				uint64_t guest_event);

/* commands.c */
struct dxghost_msg {
	struct dxghost *h;
	uint32_t process;	/* hdr.process, validated */
	const uint8_t *cmd;	/* the dxgkvmb command */
	uint32_t cmd_size;
	uint8_t *res;
	uint32_t res_cap;
	uint32_t res_len;	/* bytes written */
	int32_t ntstatus;	/* for VIRTIO_DXG_ERR_NTSTATUS replies */
	uint32_t status;	/* VIRTIO_DXG_* */
	/* Running on a vCPU thread (dxghost_try_inline): a command that
	 * finds it would need map_lock sets to_lane, undoing what it did. */
	bool inline_call;
	bool to_lane;
};

void dxghost_exec_global(struct dxghost_msg *m, uint32_t type);
void dxghost_exec_vgpu(struct dxghost_msg *m, uint32_t type);
void dxghost_destroy_process_objects(struct dxghost *h, uint32_t process);
void dxghost_sysmem_release(struct dxghost *h);
void dxghost_waits_close(struct dxghost *h);

#endif /* DXGHOST_HOST_H */
