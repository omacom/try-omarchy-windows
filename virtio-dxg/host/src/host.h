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

struct dxghost_obj {
	uint32_t handle;
	uint8_t type;
	uint8_t state;		/* 0 empty, 1 live, 2 deleted */
	uint8_t cached;		/* Lock2 view is write-back cacheable */
	uint8_t reserved;
	uint32_t process;	/* guest process token */
	uint32_t parent;	/* device; context for hwqueues; queue for fences */
	uint32_t resource;	/* allocations created with a resource */
	/* Lock2 mapping (allocation) or mapped fence page (sync, queues). */
	void *hva;
	uint64_t gpa;
	uint64_t size;
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

/* GPU VA range a guest process reserved; guest VA operations stay inside. */
struct dxghost_va {
	uint32_t process;
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
	SRWLOCK lock;		/* tables, IO space, adapter state */

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

	SRWLOCK wait_lock;
	struct dxghost_wait *waits;
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

void dxghost_io_reset(struct dxghost *h, uint64_t base, uint64_t size);
uint64_t dxghost_io_alloc(struct dxghost *h, uint64_t size);
void dxghost_io_free(struct dxghost *h, uint64_t gpa, uint64_t size);

/*
 * Maps the page holding a fence at hva into the IO space and returns the
 * fence's exact guest-physical address (page + offset), or 0.
 */
uint64_t dxghost_fence_map(struct dxghost *h, void *fence_hva);
void dxghost_fence_unmap(struct dxghost *h, uint64_t fence_gpa);

/* dxghost.c */
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
};

void dxghost_exec_global(struct dxghost_msg *m, uint32_t type);
void dxghost_exec_vgpu(struct dxghost_msg *m, uint32_t type);
void dxghost_destroy_process_objects(struct dxghost *h, uint32_t process);
void dxghost_waits_close(struct dxghost *h);

#endif /* DXGHOST_HOST_H */
