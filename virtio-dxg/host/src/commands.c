// SPDX-License-Identifier: GPL-2.0
/*
 * dxgkrnl command execution with D3DKMT.
 *
 * Each handler reads one dxgkvmb_command_*, checks every handle it names
 * against the object table, rebuilds the D3DKMT arguments with host
 * pointers (the guest's own pointer fields are never used) and writes the
 * reply layout dxgvmbus.c expects.
 *
 * Requests of one guest process arrive in order (dxghost_lane), so the
 * table can be checked before a D3DKMT call and updated after it without
 * holding the lock across the call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

#define PAGE_4K 4096ull

#define LOCK(h) AcquireSRWLockExclusive(&(h)->lock)
#define UNLOCK(h) ReleaseSRWLockExclusive(&(h)->lock)

/* Declares c, the command, or fails the request when it is truncated. */
#define COMMAND(type)							\
	const struct type *c = (const void *)m->cmd;			\
	if (m->cmd_size < sizeof(*c)) {					\
		m->status = VIRTIO_DXG_ERR_INVALID;			\
		return;							\
	}

static void *reply(struct dxghost_msg *m, uint32_t size)
{
	if (size > m->res_cap) {
		m->status = VIRTIO_DXG_ERR_OVERFLOW;
		return NULL;
	}
	m->res_len = size;
	return m->res;
}

static void reply_ntstatus(struct dxghost_msg *m, int32_t status)
{
	struct ntstatus *s;

	if (!m->res_cap)
		return;	/* async message */
	s = reply(m, sizeof(*s));
	if (s)
		s->v = status;
}

/* For replies with no NTSTATUS slot: the transport carries the failure. */
static void fail_ntstatus(struct dxghost_msg *m, int32_t status)
{
	if (status < 0) {
		m->status = VIRTIO_DXG_ERR_NTSTATUS;
		m->ntstatus = status;
	}
}

static const void *tail(struct dxghost_msg *m, uint64_t offset, uint64_t size)
{
	if (offset > m->cmd_size || size > m->cmd_size - offset) {
		m->status = VIRTIO_DXG_ERR_INVALID;
		return NULL;
	}
	return m->cmd + offset;
}

static bool owns(struct dxghost *h, uint32_t handle, uint8_t type,
		 uint32_t process)
{
	bool ok;

	LOCK(h);
	ok = dxghost_obj_get(h, handle, type, process) != NULL;
	UNLOCK(h);
	return ok;
}

static bool owns_optional(struct dxghost *h, uint32_t handle, uint8_t type,
			  uint32_t process)
{
	return !handle || owns(h, handle, type, process);
}

/* Each handle names an allocation (or, with resources, a resource). */
static bool owns_all(struct dxghost *h, const struct d3dkmthandle *list,
		     uint32_t count, uint8_t type, uint32_t process)
{
	uint32_t i;
	bool ok = true;

	LOCK(h);
	for (i = 0; i < count && ok; i++)
		ok = dxghost_obj_get(h, list[i].v, type, process) != NULL;
	UNLOCK(h);
	return ok;
}

static bool is_adapter(struct dxghost *h, uint32_t handle)
{
	return handle && handle == h->adapter;
}

static bool va_inside(struct dxghost *h, uint32_t process, uint64_t base,
		      uint64_t size)
{
	uint32_t i;
	bool ok = false;

	if (size == 0 || base + size < base)
		return false;
	LOCK(h);
	for (i = 0; i < h->va_count && !ok; i++) {
		const struct dxghost_va *v = &h->vas[i];

		ok = v->process == process && base >= v->base &&
		     base + size <= v->base + v->size;
	}
	UNLOCK(h);
	return ok;
}

static void va_add(struct dxghost *h, uint32_t process, uint64_t base,
		   uint64_t size)
{
	LOCK(h);
	if (h->va_count == h->va_capacity) {
		uint32_t capacity = h->va_capacity ? h->va_capacity * 2 : 64;
		struct dxghost_va *v = realloc(h->vas, capacity * sizeof(*v));

		if (!v) {
			UNLOCK(h);
			return;
		}
		h->vas = v;
		h->va_capacity = capacity;
	}
	h->vas[h->va_count].process = process;
	h->vas[h->va_count].base = base;
	h->vas[h->va_count].size = size;
	h->va_count++;
	UNLOCK(h);
}

/* Removes reservations of a process fully inside [base, base + size). */
static void va_remove(struct dxghost *h, uint32_t process, uint64_t base,
		      uint64_t size)
{
	uint32_t i = 0;

	LOCK(h);
	while (i < h->va_count) {
		struct dxghost_va *v = &h->vas[i];

		if (v->process == process &&
		    (size == UINT64_MAX ||
		     (v->base >= base && v->base + v->size <= base + size)))
			*v = h->vas[--h->va_count];
		else
			i++;
	}
	UNLOCK(h);
}

/* Unmaps an allocation's Lock2 view. Lock held. */
static void unmap_lock(struct dxghost *h, struct dxghost_obj *o)
{
	if (!o->gpa)
		return;
	h->vmm.unmap(h->vmm.opaque, o->gpa, o->size);
	dxghost_io_free(h, o->gpa, o->size);
	o->gpa = 0;
	o->hva = NULL;
	o->size = 0;
}

/*
 * Forgets an object and everything dxgkrnl destroys with it: children of
 * devices, contexts (hardware queues), queues (their fences) and resources.
 * Lock held.
 */
static void forget(struct dxghost *h, uint32_t handle)
{
	struct dxghost_objtab *t = &h->objects;
	struct dxghost_obj *o = dxghost_obj_find(h, handle);
	uint32_t i;

	if (!o)
		return;
	if (o->type == OBJ_ALLOCATION)
		unmap_lock(h, o);
	else if (o->gpa)
		dxghost_fence_unmap(h, o->gpa);
	o->gpa = 0;
	dxghost_obj_remove(h, handle);
	for (i = 0; i < t->capacity; i++) {
		struct dxghost_obj *child = &t->slots[i];

		if (child->state == 1 &&
		    (child->parent == handle || child->resource == handle))
			forget(h, child->handle);
	}
}

static void forget_locked(struct dxghost *h, uint32_t handle)
{
	LOCK(h);
	forget(h, handle);
	UNLOCK(h);
}

/* --- Global channel ------------------------------------------------- */

static void cmd_create_process(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createprocess);
	struct dxgkvmb_command_createprocess_return *r;
	struct dxghost *h = m->h;
	uint32_t token = 0;

	r = reply(m, sizeof(*r));
	if (!r)
		return;
	LOCK(h);
	if (h->proc_count == h->proc_capacity) {
		uint32_t capacity = h->proc_capacity ? h->proc_capacity * 2 : 64;
		struct dxghost_proc *p = realloc(h->procs,
						 capacity * sizeof(*p));

		if (p) {
			h->procs = p;
			h->proc_capacity = capacity;
		}
	}
	if (h->proc_count < h->proc_capacity) {
		/* Never zero, never reused within a VM lifetime. */
		token = 0x40000000u | (h->next_proc++ & 0x3fffffffu);
		h->procs[h->proc_count].token = token;
		h->procs[h->proc_count].guest_pid = c->process_id;
		h->procs[h->proc_count].live = true;
		h->proc_count++;
	}
	UNLOCK(h);
	r->hprocess.v = token;	/* 0 tells the guest the host is out of room */
}

void dxghost_destroy_process_objects(struct dxghost *h, uint32_t process)
{
	struct dxghost_objtab *t;
	uint32_t i;

	/* Devices first: dxgkrnl destroys their children with them. */
	LOCK(h);
	t = &h->objects;
	for (i = 0; i < t->capacity; i++) {
		struct dxghost_obj *o = &t->slots[i];
		uint32_t handle = o->handle;

		if (o->state != 1 || o->process != process ||
		    o->type != OBJ_DEVICE)
			continue;
		forget(h, handle);
		UNLOCK(h);
		{
			struct d3dkmt_destroydevice d = {0};

			d.device.v = handle;
			h->kmt.DestroyDevice(&d);
		}
		LOCK(h);
	}
	/* Then device-less sync objects. */
	for (i = 0; i < t->capacity; i++) {
		struct dxghost_obj *o = &t->slots[i];
		uint32_t handle = o->handle;

		if (o->state != 1 || o->process != process)
			continue;
		forget(h, handle);
		if (o->type == OBJ_SYNC && h->kmt.DestroySynchronizationObject) {
			struct d3dkmt_destroysynchronizationobject d = {0};

			d.sync_object.v = handle;
			UNLOCK(h);
			h->kmt.DestroySynchronizationObject(&d);
			LOCK(h);
		}
	}
	UNLOCK(h);
	va_remove(h, process, 0, UINT64_MAX);
}

static void cmd_destroy_process(struct dxghost_msg *m)
{
	struct dxghost *h = m->h;
	uint32_t i;

	dxghost_destroy_process_objects(h, m->process);
	LOCK(h);
	for (i = 0; i < h->proc_count; i++) {
		if (h->procs[i].token == m->process) {
			h->procs[i] = h->procs[--h->proc_count];
			break;
		}
	}
	UNLOCK(h);
	reply_ntstatus(m, DXG_STATUS_SUCCESS);
}

static void cmd_set_iospace(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_setiospaceregion);
	struct dxghost *h = m->h;
	int32_t status = DXG_STATUS_SUCCESS;

	LOCK(h);
	if (!c->length || c->length > h->config.iospace_size ||
	    (c->start | c->length) & (PAGE_4K - 1))
		status = DXG_STATUS_INVALID_PARAMETER;
	else if (h->io_size && (h->io_base != c->start ||
				h->io_size != c->length))
		status = DXG_STATUS_ACCESS_DENIED;	/* set once per boot */
	else if (!h->io_size)
		dxghost_io_reset(h, c->start, c->length);
	UNLOCK(h);
	reply_ntstatus(m, status);
}

static void cmd_destroy_sync(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroysyncobject);
	struct dxghost *h = m->h;
	struct d3dkmt_destroysynchronizationobject d = {0};
	int32_t status;

	if (!owns(h, c->sync_object.v, OBJ_SYNC, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	d.sync_object = c->sync_object;
	status = h->kmt.DestroySynchronizationObject(&d);
	forget_locked(h, c->sync_object.v);
	reply_ntstatus(m, status);
}

static void cmd_feature_global(struct dxghost_msg *m)
{
	struct dxgkvmb_command_isfeatureenabled_return *r =
		reply(m, sizeof(*r));

	/* No optional dxgkrnl features are offered yet. */
	if (r)
		r->status.v = DXG_STATUS_NOT_SUPPORTED;
}

void dxghost_exec_global(struct dxghost_msg *m, uint32_t type)
{
	switch (type) {
	case DXGK_VMBCOMMAND_CREATEPROCESS:
		cmd_create_process(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYPROCESS:
		cmd_destroy_process(m);
		break;
	case DXGK_VMBCOMMAND_SETIOSPACEREGION:
		cmd_set_iospace(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYSYNCOBJECT:
		cmd_destroy_sync(m);
		break;
	case DXGK_VMBCOMMAND_ISFEATUREENABLED_GLOBAL:
		cmd_feature_global(m);
		break;
	case DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT:
		/* Cross-process sharing is not offered yet: handle 0. */
		reply(m, sizeof(struct d3dkmthandle));
		break;
	case DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT:
	case DXGK_VMBCOMMAND_OPENSYNCOBJECT:
	case DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST:
		/* Every reply here begins with an NTSTATUS. */
		reply_ntstatus(m, DXG_STATUS_NOT_SUPPORTED);
		if (m->res_len && m->res_cap > m->res_len)
			m->res_len = m->res_cap;
		break;
	default:
		m->status = VIRTIO_DXG_ERR_UNSUPPORTED;
		break;
	}
}

/* --- Adapter ---------------------------------------------------------- */

static void cmd_open_adapter(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_openadapter);
	struct dxgkvmb_command_openadapter_return *r = reply(m, sizeof(*r));
	struct dxghost *h = m->h;
	int32_t status = DXG_STATUS_SUCCESS;

	if (!r)
		return;
	LOCK(h);
	if (!h->adapter) {
		struct d3dkmt_openadapterfromluid open = {0};

		open.adapter_luid.a = h->luid.LowPart;
		open.adapter_luid.b = (u32)h->luid.HighPart;
		status = h->kmt.OpenAdapterFromLuid(&open);
		if (status >= 0)
			h->adapter = open.adapter_handle.v;
	}
	if (status >= 0)
		h->adapter_refs++;
	r->host_adapter_handle.v = h->adapter;
	UNLOCK(h);
	r->status.v = status;
	r->vmbus_interface_version =
		c->vmbus_interface_version < DXGHOST_INTERFACE_VERSION ?
		c->vmbus_interface_version : DXGHOST_INTERFACE_VERSION;
	r->vmbus_last_compatible_interface_version =
		DXGHOST_LAST_COMPATIBLE_VERSION;
}

static void cmd_close_adapter(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_closeadapter);
	struct dxghost *h = m->h;

	LOCK(h);
	if (is_adapter(h, c->host_handle.v) && h->adapter_refs &&
	    --h->adapter_refs == 0) {
		struct d3dkmt_closeadapter close = {0};

		close.adapter_handle.v = h->adapter;
		h->kmt.CloseAdapter(&close);
		h->adapter = 0;
	}
	UNLOCK(h);
}

static int32_t query(struct dxghost *h, uint32_t type, void *data,
		     uint32_t size)
{
	struct d3dkmt_queryadapterinfo q = {0};

	q.adapter.v = h->adapter;
	q.type = type;
	q.private_data = (u64)(uintptr_t)data;
	q.private_data_size = size;
	return h->kmt.QueryAdapterInfo(&q);
}

static void cmd_internal_adapter_info(struct dxghost_msg *m)
{
	struct dxgkvmb_command_getinternaladapterinfo_return *r;
	struct dxghost *h = m->h;
	uint32_t ddi_version = 0, caps = 0, n;
	static wchar_t description[4096];
	static SRWLOCK description_lock = SRWLOCK_INIT;

	/* Older protocol versions omit the trailing vGPU LUID. */
	n = m->res_cap < sizeof(*r) ? m->res_cap : sizeof(*r);
	if (n < sizeof(*r) - sizeof(struct winluid)) {
		m->status = VIRTIO_DXG_ERR_OVERFLOW;
		return;
	}
	r = reply(m, n);
	query(h, DXGHOST_QAI_DRIVERVERSION, &ddi_version, sizeof(ddi_version));
	query(h, DXGHOST_QAI_WDDM_2_7_CAPS, &caps, sizeof(caps));
	r->driver_ddi_version = ddi_version;
	r->hw_sch_capable = caps & 1;		/* HwSchSupported */
	r->hw_sch_enabled = (caps >> 1) & 1;	/* HwSchEnabled */
	r->async_msg_enabled = 1;
	r->host_adapter_luid.a = h->luid.LowPart;
	r->host_adapter_luid.b = (u32)h->luid.HighPart;
	AcquireSRWLockExclusive(&description_lock);
	if (query(h, DXGHOST_QAI_DRIVER_DESCRIPTION, description,
		  sizeof(description)) >= 0) {
		for (n = 0; n < 79 && description[n]; n++)
			r->device_description[n] = (u16)description[n];
	}
	ReleaseSRWLockExclusive(&description_lock);
	if (m->res_len == sizeof(*r)) {
		r->host_vgpu_luid.a = h->luid.LowPart;
		r->host_vgpu_luid.b = (u32)h->luid.HighPart;
	}
}

/*
 * The Windows UMD path names a .dll; the guest needs the Linux build that
 * NVIDIA ships beside it: nvwgf2umx.dll -> libnvwgf2umx.so.
 */
static void adapt_umd_name(struct dxghost *h, u8 *data, uint32_t size)
{
	u16 *path = (u16 *)(data + sizeof(u32));
	const uint32_t max = 260;
	uint32_t len = 0, base = 0, i;
	char name[128];

	if (size < sizeof(u32) + max * sizeof(u16))
		return;
	while (len < max && path[len]) {
		if (path[len] == '\\' || path[len] == '/')
			base = len + 1;
		len++;
	}
	if (len == max)
		return;
	if (h->config.linux_umd_name) {
		snprintf(name, sizeof(name), "%s", h->config.linux_umd_name);
	} else {
		char windows[96];
		uint32_t n = 0;

		for (i = base; i < len && n + 1 < sizeof(windows); i++)
			windows[n++] = (char)path[i];
		windows[n] = 0;
		if (n > 4 && !_stricmp(windows + n - 4, ".dll"))
			windows[n - 4] = 0;
		snprintf(name, sizeof(name), "lib%s.so", windows);
	}
	for (i = 0; name[i] && base + i < max - 1; i++)
		path[base + i] = (u16)name[i];
	for (; base + i < max; i++)
		path[base + i] = 0;
}

static void cmd_query_adapter_info(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_queryadapterinfo);
	uint32_t size = c->private_data_size;
	const u8 *in = tail(m, offsetof(struct dxgkvmb_command_queryadapterinfo,
					private_data), size);
	struct ntstatus *status;
	u8 *out;

	if (!in)
		return;
	status = reply(m, sizeof(*status) + size);
	if (!status)
		return;
	out = (u8 *)(status + 1);
	memcpy(out, in, size);	/* queries can carry input data */
	status->v = query(m->h, c->query_type, out, size);
	if (status->v >= 0 && c->query_type == DXGHOST_QAI_UMDRIVERNAME)
		adapt_umd_name(m->h, out, size);
}

static void cmd_feature(struct dxghost_msg *m)
{
	struct dxgkvmb_command_isfeatureenabled_return *r =
		reply(m, sizeof(*r));

	if (r)
		r->status.v = DXG_STATUS_NOT_SUPPORTED;
}

/* --- Devices, contexts, paging queues -------------------------------- */

static void cmd_create_device(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createdevice);
	struct dxgkvmb_command_createdevice_return *r = reply(m, sizeof(*r));
	struct d3dkmt_createdevice d = {0};
	struct dxghost *h = m->h;

	if (!r || !m->process || !h->adapter)
		return;
	d.adapter.v = h->adapter;
	d.flags = c->flags;
	if (h->kmt.CreateDevice(&d) < 0)
		return;	/* device 0 reports the failure */
	LOCK(h);
	if (dxghost_obj_add(h, d.device.v, OBJ_DEVICE, m->process, 0)) {
		struct d3dkmt_destroydevice destroy = {0};

		UNLOCK(h);
		destroy.device = d.device;
		h->kmt.DestroyDevice(&destroy);
		return;
	}
	UNLOCK(h);
	r->device = d.device;
}

static void cmd_destroy_device(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroydevice);
	struct d3dkmt_destroydevice d = {0};
	struct dxghost *h = m->h;

	if (!owns(h, c->device.v, OBJ_DEVICE, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	/* Unmap first: the views die with the device. */
	forget_locked(h, c->device.v);
	d.device = c->device;
	reply_ntstatus(m, h->kmt.DestroyDevice(&d));
}

static void cmd_create_context(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createcontextvirtual);
	uint32_t size = c->priv_drv_data_size;
	uint32_t total = (uint32_t)offsetof(
		struct dxgkvmb_command_createcontextvirtual, priv_drv_data) + size;
	struct dxgkvmb_command_createcontextvirtual *r;
	struct d3dkmt_createcontextvirtual a = {0};
	struct dxghost *h = m->h;

	if (!tail(m, 0, total))
		return;
	/* The reply is the command, with context and private data updated. */
	r = reply(m, total < sizeof(*r) ? (uint32_t)sizeof(*r) : total);
	if (!r)
		return;
	memcpy(r, c, total);
	r->context.v = 0;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process))
		return;
	a.device = c->device;
	a.node_ordinal = c->node_ordinal;
	a.engine_affinity = c->engine_affinity;
	a.flags = c->flags;
	a.priv_drv_data = size ? (u64)(uintptr_t)r->priv_drv_data : 0;
	a.priv_drv_data_size = size;
	a.client_hint = c->client_hint;
	if (h->kmt.CreateContextVirtual(&a) < 0)
		return;
	LOCK(h);
	if (dxghost_obj_add(h, a.context.v, OBJ_CONTEXT, m->process,
			    c->device.v) == 0)
		r->context = a.context;
	UNLOCK(h);
	if (!r->context.v) {
		struct d3dkmt_destroycontext d = {0};

		d.context = a.context;
		h->kmt.DestroyContext(&d);
	}
}

static void cmd_destroy_context(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroycontext);
	struct d3dkmt_destroycontext d = {0};
	struct dxghost *h = m->h;

	if (!owns(h, c->context.v, OBJ_CONTEXT, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	forget_locked(h, c->context.v);
	d.context = c->context;
	reply_ntstatus(m, h->kmt.DestroyContext(&d));
}

static void cmd_create_paging_queue(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createpagingqueue);
	struct dxgkvmb_command_createpagingqueue_return *r =
		reply(m, sizeof(*r));
	struct d3dkmt_createpagingqueue a = c->args;
	struct dxghost *h = m->h;
	struct dxghost_obj *o;
	uint64_t gpa;

	if (!r || !owns(h, a.device.v, OBJ_DEVICE, m->process))
		return;
	a.fence_cpu_virtual_address = 0;
	if (h->kmt.CreatePagingQueue(&a) < 0)
		return;
	LOCK(h);
	gpa = dxghost_fence_map(h, (void *)(uintptr_t)a.fence_cpu_virtual_address);
	if (gpa && !dxghost_obj_add(h, a.paging_queue.v, OBJ_PAGING_QUEUE,
				    m->process, a.device.v)) {
		o = dxghost_obj_find(h, a.paging_queue.v);
		o->gpa = gpa;
		/* The queue's fence dies with the queue. */
		dxghost_obj_add(h, a.sync_object.v, OBJ_SYNC, m->process,
				a.paging_queue.v);
		r->paging_queue = a.paging_queue;
		r->sync_object = a.sync_object;
		r->fence_storage_physical_address = gpa;
		r->fence_storage_offset = gpa & (PAGE_4K - 1);
	} else if (gpa) {
		dxghost_fence_unmap(h, gpa);
	}
	UNLOCK(h);
	if (!r->paging_queue.v) {
		struct { struct d3dkmthandle q; } d = {a.paging_queue};

		h->kmt.DestroyPagingQueue(&d);
	}
}

static void cmd_destroy_paging_queue(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroypagingqueue);
	struct { struct d3dkmthandle q; } d = {c->paging_queue};
	struct dxghost *h = m->h;

	if (!owns(h, c->paging_queue.v, OBJ_PAGING_QUEUE, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	forget_locked(h, c->paging_queue.v);
	reply_ntstatus(m, h->kmt.DestroyPagingQueue(&d));
}

/* --- Allocations ------------------------------------------------------ */

static void cmd_create_allocation(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createallocation);
	const struct dxgkvmb_command_createallocation_allocinfo *info;
	struct dxgkvmb_command_createallocation_return *r;
	struct dxgkvmb_command_allocinfo_return *ri;
	struct d3dddi_allocationinfo2 *a2 = NULL;
	struct d3dkmt_createallocation a = {0};
	struct dxghost *h = m->h;
	uint32_t count = c->alloc_count, i;
	uint64_t offset, alloc_priv = 0, result_size;
	const u8 *runtime, *global;
	u8 *out_priv;

	if (!count || count > D3DKMT_CREATEALLOCATION_MAX ||
	    c->flags.existing_sysmem || c->flags.standard_allocation ||
	    c->flags.existing_section) {
		/* Existing-memory allocations need guest page mapping. */
		m->status = count ? VIRTIO_DXG_ERR_NTSTATUS :
				    VIRTIO_DXG_ERR_INVALID;
		m->ntstatus = DXG_STATUS_NOT_SUPPORTED;
		return;
	}
	info = tail(m, sizeof(*c), (uint64_t)count * sizeof(*info));
	if (!info)
		return;
	offset = sizeof(*c) + (uint64_t)count * sizeof(*info);
	runtime = tail(m, offset, c->private_runtime_data_size);
	offset += c->private_runtime_data_size;
	global = tail(m, offset, c->priv_drv_data_size);
	offset += c->priv_drv_data_size;
	if (!runtime || !global)
		return;
	for (i = 0; i < count; i++)
		alloc_priv += info[i].priv_drv_data_size;
	if (!tail(m, offset, alloc_priv))
		return;

	result_size = sizeof(*r) + (count - 1) * sizeof(*ri) + alloc_priv;
	r = reply(m, (uint32_t)result_size);
	if (!r)
		return;
	ri = r->allocation_info;
	out_priv = (u8 *)&ri[count];
	/* Per-allocation private data is in/out; the KMD may rewrite it. */
	memcpy(out_priv, m->cmd + offset, alloc_priv);

	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    (c->resource.v && !c->flags.create_resource &&
	     !owns(h, c->resource.v, OBJ_RESOURCE, m->process))) {
		fail_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a2 = calloc(count, sizeof(*a2));
	if (!a2) {
		fail_ntstatus(m, DXG_STATUS_NO_MEMORY);
		return;
	}
	for (i = 0, offset = 0; i < count; i++) {
		a2[i].priv_drv_data = info[i].priv_drv_data_size ?
			(u64)(uintptr_t)(out_priv + offset) : 0;
		a2[i].priv_drv_data_size = info[i].priv_drv_data_size;
		a2[i].vidpn_source_id = info[i].vidpn_source_id;
		a2[i].flags.value = info[i].flags;
		offset += info[i].priv_drv_data_size;
	}
	a.device = c->device;
	a.resource = c->flags.create_resource ? (struct d3dkmthandle){0} :
						 c->resource;
	a.private_runtime_data = c->private_runtime_data_size ?
				 (u64)(uintptr_t)runtime : 0;
	a.private_runtime_data_size = c->private_runtime_data_size;
	a.priv_drv_data = c->priv_drv_data_size ? (u64)(uintptr_t)global : 0;
	a.priv_drv_data_size = c->priv_drv_data_size;
	a.alloc_count = count;
	a.allocation_info = (u64)(uintptr_t)a2;
	a.flags = c->flags;
	a.private_runtime_resource_handle = c->private_runtime_resource_handle;

	m->ntstatus = h->kmt.CreateAllocation2(&a);
	if (m->ntstatus < 0) {
		free(a2);
		fail_ntstatus(m, m->ntstatus);
		return;
	}
	m->ntstatus = 0;

	LOCK(h);
	if (c->flags.create_resource)
		dxghost_obj_add(h, a.resource.v, OBJ_RESOURCE, m->process,
				c->device.v);
	for (i = 0; i < count; i++) {
		dxghost_obj_add(h, a2[i].allocation.v, OBJ_ALLOCATION,
				m->process, c->device.v);
		if (a.resource.v)
			dxghost_obj_find(h, a2[i].allocation.v)->resource =
				a.resource.v;
		ri[i].allocation = a2[i].allocation;
		ri[i].priv_drv_data_size = info[i].priv_drv_data_size;
		/*
		 * Size and caching are reported at Lock2: user mode cannot
		 * read the KMD's allocation description.
		 */
	}
	UNLOCK(h);
	r->flags = c->flags;
	r->resource = a.resource;
	r->global_share = a.global_share;
	free(a2);
}

static void cmd_destroy_allocation(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroyallocation);
	const struct d3dkmthandle *list = c->allocations;
	struct d3dkmt_destroyallocation2 d = {0};
	struct dxghost *h = m->h;
	uint32_t i;

	if (c->alloc_count &&
	    !tail(m, offsetof(struct dxgkvmb_command_destroyallocation,
			      allocations),
		  (uint64_t)c->alloc_count * sizeof(*list)))
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_optional(h, c->resource.v, OBJ_RESOURCE, m->process) ||
	    !owns_all(h, list, c->alloc_count, OBJ_ALLOCATION, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	LOCK(h);
	for (i = 0; i < c->alloc_count; i++)
		forget(h, list[i].v);
	if (c->resource.v)
		forget(h, c->resource.v);
	UNLOCK(h);
	d.device = c->device;
	d.resource = c->resource;
	d.allocations = c->alloc_count ? (u64)(uintptr_t)list : 0;
	d.alloc_count = c->alloc_count;
	d.flags = c->flags;
	reply_ntstatus(m, h->kmt.DestroyAllocation2(&d));
}

static void cmd_lock2(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_lock2);
	struct dxgkvmb_command_lock2_return_virtio *r;
	struct d3dkmt_lock2 a = c->args;
	struct dxghost *h = m->h;
	struct dxghost_obj *o;
	MEMORY_BASIC_INFORMATION mbi;
	uint64_t gpa, size;
	bool cached;

	r = reply(m, m->res_cap >= sizeof(*r) ? sizeof(*r) : sizeof(r->base));
	if (!r)
		return;
	if (!owns(h, a.device.v, OBJ_DEVICE, m->process) ||
	    !owns(h, a.allocation.v, OBJ_ALLOCATION, m->process)) {
		r->base.status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	/* Already visible: the guest keeps one mapping per allocation. */
	LOCK(h);
	o = dxghost_obj_find(h, a.allocation.v);
	if (o->gpa) {
		r->base.cpu_visible_buffer_offset = (void *)(uintptr_t)o->gpa;
		r->mapped_size = o->size;
		r->flags = o->cached ? DXGK_VIRTIO_LOCK2_CACHED : 0;
		UNLOCK(h);
		return;
	}
	UNLOCK(h);

	a.data = 0;
	r->base.status.v = h->kmt.Lock2(&a);
	if (r->base.status.v < 0)
		return;
	if (!VirtualQuery((void *)(uintptr_t)a.data, &mbi, sizeof(mbi)) ||
	    mbi.State != MEM_COMMIT) {
		goto unlock;
	}
	size = (uint64_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize -
			  (uintptr_t)a.data);
	size = (size + PAGE_4K - 1) & ~(PAGE_4K - 1);
	cached = !(mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE));

	LOCK(h);
	gpa = dxghost_io_alloc(h, size);
	/* Lock2 has no read-only flag: guest views are always writable. */
	if (gpa && h->vmm.map(h->vmm.opaque, gpa, (void *)(uintptr_t)a.data,
			      size, true)) {
		dxghost_io_free(h, gpa, size);
		gpa = 0;
	}
	if (gpa) {
		o = dxghost_obj_find(h, a.allocation.v);
		o->hva = (void *)(uintptr_t)a.data;
		o->gpa = gpa;
		o->size = size;
		o->cached = cached;
	}
	UNLOCK(h);
	if (!gpa) {
		dxghost_logf(h, "IO space exhausted mapping %llu bytes",
			     (unsigned long long)size);
		goto unlock;
	}
	r->base.cpu_visible_buffer_offset = (void *)(uintptr_t)gpa;
	r->mapped_size = size;
	r->flags = cached ? DXGK_VIRTIO_LOCK2_CACHED : 0;
	return;
unlock:
	{
		struct d3dkmt_unlock2 u = {a.device, a.allocation};

		h->kmt.Unlock2(&u);
		r->base.status.v = DXG_STATUS_NO_MEMORY;
	}
}

static void cmd_unlock2(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_unlock2);
	struct d3dkmt_unlock2 u = c->args;
	struct dxghost *h = m->h;
	struct dxghost_obj *o;
	bool mapped = false;

	if (!owns(h, u.device.v, OBJ_DEVICE, m->process) ||
	    !owns(h, u.allocation.v, OBJ_ALLOCATION, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	LOCK(h);
	o = dxghost_obj_find(h, u.allocation.v);
	if (o->gpa) {
		unmap_lock(h, o);
		mapped = true;
	}
	UNLOCK(h);
	reply_ntstatus(m, mapped ? h->kmt.Unlock2(&u) : DXG_STATUS_SUCCESS);
}

/* --- Residency and GPU virtual addresses ----------------------------- */

static void cmd_make_resident(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_makeresident);
	struct dxgkvmb_command_makeresident_return *r = reply(m, sizeof(*r));
	struct d3dddi_makeresident a = {0};
	struct dxghost *h = m->h;
	const struct d3dkmthandle *list = c->allocations;

	if (!r || !tail(m, offsetof(struct dxgkvmb_command_makeresident,
				    allocations),
			(uint64_t)c->alloc_count * sizeof(*list)))
		return;
	if (!owns(h, c->paging_queue.v, OBJ_PAGING_QUEUE, m->process) ||
	    !owns_all(h, list, c->alloc_count, OBJ_ALLOCATION, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.paging_queue = c->paging_queue;
	a.alloc_count = c->alloc_count;
	a.allocation_list = (u64)(uintptr_t)list;
	a.flags = c->flags;
	r->status.v = h->kmt.MakeResident(&a);
	r->paging_fence_value = a.paging_fence_value;
	r->num_bytes_to_trim = a.num_bytes_to_trim;
}

static void cmd_evict(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_evict);
	struct dxgkvmb_command_evict_return *r = reply(m, sizeof(*r));
	struct d3dkmt_evict a = {0};
	struct dxghost *h = m->h;
	const struct d3dkmthandle *list = c->allocations;

	if (!r || !tail(m, offsetof(struct dxgkvmb_command_evict, allocations),
			(uint64_t)c->alloc_count * sizeof(*list)))
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_all(h, list, c->alloc_count, OBJ_ALLOCATION, m->process)) {
		fail_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.device = c->device;
	a.alloc_count = c->alloc_count;
	a.allocations = (u64)(uintptr_t)list;
	a.flags = c->flags;
	fail_ntstatus(m, h->kmt.Evict(&a));
	r->num_bytes_to_trim = a.num_bytes_to_trim;
}

static void cmd_reserve_gpu_va(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_reservegpuvirtualaddress);
	struct dxgkvmb_command_reservegpuvirtualaddress_return *r =
		reply(m, sizeof(*r));
	struct d3dddi_reservegpuvirtualaddress a = c->args;
	struct dxghost *h = m->h;
	int32_t status;

	if (!r)
		return;
	/* The first handle is the adapter or a paging queue. */
	if (!is_adapter(h, a.adapter.v) &&
	    !owns(h, a.adapter.v, OBJ_PAGING_QUEUE, m->process)) {
		fail_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	/*
	 * A fixed base may not land inside the VMM's own reservations, which
	 * share this process's GPU address space.
	 */
	status = h->kmt.ReserveGpuVirtualAddress(&a);
	fail_ntstatus(m, status);
	if (status < 0)
		return;
	va_add(h, m->process, a.virtual_address, a.size);
	r->virtual_address = a.virtual_address;
	r->paging_fence_value = a.paging_fence_value;
}

static void cmd_free_gpu_va(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_freegpuvirtualaddress);
	struct d3dkmt_freegpuvirtualaddress a = c->args;
	struct dxghost *h = m->h;

	if (!is_adapter(h, a.adapter.v) ||
	    !va_inside(h, m->process, a.base_address, a.size)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_PARAMETER);
		return;
	}
	va_remove(h, m->process, a.base_address, a.size);
	reply_ntstatus(m, h->kmt.FreeGpuVirtualAddress(&a));
}

static void cmd_map_gpu_va(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_mapgpuvirtualaddress);
	struct dxgkvmb_command_mapgpuvirtualaddress_return *r =
		reply(m, sizeof(*r));
	struct d3dddi_mapgpuvirtualaddress a = c->args;
	struct dxghost *h = m->h;

	if (!r)
		return;
	if (!owns(h, a.paging_queue.v, OBJ_PAGING_QUEUE, m->process) ||
	    !owns(h, a.allocation.v, OBJ_ALLOCATION, m->process) ||
	    (a.base_address &&
	     !va_inside(h, m->process, a.base_address,
			a.size_in_pages * PAGE_4K))) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	r->status.v = h->kmt.MapGpuVirtualAddress(&a);
	r->virtual_address = a.virtual_address;
	r->paging_fence_value = a.paging_fence_value;
	/* Maps without a base allocate their own range. */
	if (r->status.v >= 0 && !a.base_address)
		va_add(h, m->process, a.virtual_address,
		       a.size_in_pages * PAGE_4K);
}

static void cmd_update_gpu_va(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_updategpuvirtualaddress);
	const struct d3dddi_updategpuvirtualaddress_operation *ops =
		c->operations;
	struct d3dkmt_updategpuvirtualaddress a = {0};
	struct dxghost *h = m->h;
	uint32_t i;

	if (!c->num_operations ||
	    !tail(m, offsetof(struct dxgkvmb_command_updategpuvirtualaddress,
			      operations),
		  (uint64_t)c->num_operations * sizeof(*ops)))
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns(h, c->context.v, OBJ_CONTEXT, m->process) ||
	    !owns(h, c->fence_object.v, OBJ_SYNC, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	for (i = 0; i < c->num_operations; i++) {
		const struct d3dddi_updategpuvirtualaddress_operation *op = &ops[i];
		bool ok;

		switch (op->operation) {
		case _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP:
		case _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT:
			ok = va_inside(h, m->process, op->map.base_address,
				       op->map.size) &&
			     owns(h, op->map.allocation.v, OBJ_ALLOCATION,
				  m->process);
			break;
		case _D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP:
			ok = va_inside(h, m->process, op->unmap.base_address,
				       op->unmap.size);
			break;
		case _D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY:
			ok = va_inside(h, m->process, op->copy.source_address,
				       op->copy.size) &&
			     va_inside(h, m->process, op->copy.dest_address,
				       op->copy.size);
			break;
		default:
			ok = false;
		}
		if (!ok) {
			reply_ntstatus(m, DXG_STATUS_INVALID_PARAMETER);
			return;
		}
	}
	a.device = c->device;
	a.context = c->context;
	a.fence_object = c->fence_object;
	a.num_operations = c->num_operations;
	a.operations = (u64)(uintptr_t)ops;
	a.fence_value = c->fence_value;
	a.flags.value = c->flags;
	reply_ntstatus(m, h->kmt.UpdateGpuVirtualAddress(&a));
}

/* --- Submission ------------------------------------------------------- */

static void cmd_submit(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_submitcommand);
	struct d3dkmt_submitcommand a = c->args;
	struct dxghost *h = m->h;
	uint64_t hsize = (uint64_t)a.num_history_buffers *
			 sizeof(struct d3dkmthandle);
	const struct d3dkmthandle *history = tail(m, sizeof(*c), hsize);
	const u8 *priv = tail(m, sizeof(*c) + hsize, a.priv_drv_data_size);
	uint32_t i;

	if (!history || !priv)
		return;
	if (a.broadcast_context_count == 0 ||
	    a.broadcast_context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
	    a.num_primaries) {
		reply_ntstatus(m, DXG_STATUS_INVALID_PARAMETER);
		return;
	}
	for (i = 0; i < a.broadcast_context_count; i++) {
		if (!owns(h, a.broadcast_context[i].v, OBJ_CONTEXT,
			  m->process)) {
			reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
			return;
		}
	}
	if (!owns_all(h, history, a.num_history_buffers, OBJ_ALLOCATION,
		      m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.history_buffer_array = a.num_history_buffers ?
				 (u64)(uintptr_t)history : 0;
	a.priv_drv_data = a.priv_drv_data_size ? (u64)(uintptr_t)priv : 0;
	reply_ntstatus(m, h->kmt.SubmitCommand(&a));
}

static void cmd_submit_hwqueue(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_submitcommandtohwqueue);
	struct d3dkmt_submitcommandtohwqueue a = c->args;
	struct dxghost *h = m->h;
	uint64_t psize = (uint64_t)a.num_primaries * sizeof(struct d3dkmthandle);
	const u8 *priv = tail(m, sizeof(*c) + psize, a.priv_drv_data_size);

	if (!priv)
		return;
	if (a.num_primaries ||
	    !owns(h, a.hwqueue.v, OBJ_HWQUEUE, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.priv_drv_data = a.priv_drv_data_size ? (u64)(uintptr_t)priv : 0;
	a.written_primaries = 0;
	reply_ntstatus(m, h->kmt.SubmitCommandToHwQueue(&a));
}

static void cmd_create_hwqueue(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createhwqueue);
	uint32_t size = c->priv_drv_data_size;
	uint32_t total = (uint32_t)offsetof(struct dxgkvmb_command_createhwqueue,
					    priv_drv_data) + size;
	struct dxgkvmb_command_createhwqueue *r;
	struct d3dkmt_createhwqueue a = {0};
	struct dxghost *h = m->h;
	uint64_t gpa = 0;

	if (!tail(m, 0, total))
		return;
	r = reply(m, total < sizeof(*r) ? (uint32_t)sizeof(*r) : total);
	if (!r)
		return;
	memcpy(r, c, total);
	r->hwqueue.v = 0;
	r->hwqueue_progress_fence.v = 0;
	r->hwqueue_progress_fence_cpuva = NULL;
	if (!owns(h, c->context.v, OBJ_CONTEXT, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.context = c->context;
	a.flags = c->flags;
	a.priv_drv_data_size = size;
	a.priv_drv_data = size ? (u64)(uintptr_t)r->priv_drv_data : 0;
	r->status.v = h->kmt.CreateHwQueue(&a);
	if (r->status.v < 0)
		return;
	LOCK(h);
	gpa = dxghost_fence_map(h, (void *)(uintptr_t)a.queue_progress_fence_cpu_va);
	if (gpa && !dxghost_obj_add(h, a.queue.v, OBJ_HWQUEUE, m->process,
				    c->context.v)) {
		dxghost_obj_find(h, a.queue.v)->gpa = gpa;
		dxghost_obj_add(h, a.queue_progress_fence.v, OBJ_SYNC,
				m->process, a.queue.v);
	} else {
		if (gpa)
			dxghost_fence_unmap(h, gpa);
		gpa = 0;
	}
	UNLOCK(h);
	if (!gpa) {
		struct d3dkmt_destroyhwqueue d = {a.queue};

		h->kmt.DestroyHwQueue(&d);
		r->status.v = DXG_STATUS_NO_MEMORY;
		return;
	}
	r->hwqueue = a.queue;
	r->hwqueue_progress_fence = a.queue_progress_fence;
	r->hwqueue_progress_fence_cpuva = (void *)(uintptr_t)gpa;
	r->hwqueue_progress_fence_gpuva = a.queue_progress_fence_gpu_va;
}

static void cmd_destroy_hwqueue(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_destroyhwqueue);
	struct d3dkmt_destroyhwqueue d = {c->hwqueue};
	struct dxghost *h = m->h;

	if (!owns(h, c->hwqueue.v, OBJ_HWQUEUE, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	forget_locked(h, c->hwqueue.v);
	reply_ntstatus(m, h->kmt.DestroyHwQueue(&d));
}

/* --- Synchronization -------------------------------------------------- */

static void cmd_create_sync(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_createsyncobject);
	struct dxgkvmb_command_createsyncobject_return *r = reply(m, sizeof(*r));
	struct d3dkmt_createsynchronizationobject2 a = c->args;
	struct dxghost *h = m->h;
	uint64_t cpu_va = 0, gpu_va = 0, gpa = 0;
	bool monitored;

	if (!r)
		return;
	if (!owns_optional(h, a.device.v, OBJ_DEVICE, m->process))
		return;
	monitored = a.info.type == _D3DDDI_MONITORED_FENCE ||
		    a.info.type == _D3DDDI_PERIODIC_MONITORED_FENCE;
	if (a.info.type == _D3DDDI_PERIODIC_MONITORED_FENCE &&
	    !is_adapter(h, a.info.periodic_monitored_fence.adapter.v))
		return;
	if (a.info.type == _D3DDDI_CPU_NOTIFICATION)
		return;	/* would need a host event per guest eventfd */
	if (h->kmt.CreateSynchronizationObject2(&a) < 0)
		return;
	if (a.info.type == _D3DDDI_MONITORED_FENCE) {
		cpu_va = (uint64_t)(uintptr_t)
			 a.info.monitored_fence.fence_cpu_virtual_address;
		gpu_va = a.info.monitored_fence.fence_gpu_virtual_address;
	} else if (monitored) {
		cpu_va = (uint64_t)(uintptr_t)
			 a.info.periodic_monitored_fence.fence_cpu_virtual_address;
		gpu_va = a.info.periodic_monitored_fence.fence_gpu_virtual_address;
	}
	LOCK(h);
	if (monitored)
		gpa = dxghost_fence_map(h, (void *)(uintptr_t)cpu_va);
	if ((!monitored || gpa) &&
	    !dxghost_obj_add(h, a.sync_object.v, OBJ_SYNC, m->process,
			     a.device.v)) {
		dxghost_obj_find(h, a.sync_object.v)->gpa = gpa;
		r->sync_object = a.sync_object;
		r->global_sync_object = a.info.shared_handle;
		r->fence_gpu_va = gpu_va;
		r->fence_storage_address = gpa;
		r->fence_storage_offset = (u32)(gpa & (PAGE_4K - 1));
	} else if (gpa) {
		dxghost_fence_unmap(h, gpa);
	}
	UNLOCK(h);
	if (!r->sync_object.v) {
		struct d3dkmt_destroysynchronizationobject d = {a.sync_object};

		h->kmt.DestroySynchronizationObject(&d);
	}
}

static void cmd_signal_sync(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_signalsyncobject);
	struct dxghost *h = m->h;
	uint64_t objects_size = (uint64_t)c->object_count * sizeof(u32);
	uint64_t contexts_size = (uint64_t)c->context_count * sizeof(u32);
	const struct d3dkmthandle *objects = tail(m, sizeof(*c), objects_size);
	const struct d3dkmthandle *contexts =
		tail(m, sizeof(*c) + objects_size, contexts_size);
	uint64_t fences_offset = sizeof(*c) + objects_size + contexts_size;
	const u64 *fences = NULL;
	int32_t status;

	if (!objects || !contexts)
		return;
	if (!c->object_count || c->object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
	    c->context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
	    c->flags.enqueue_cpu_event ||
	    !owns_all(h, objects, c->object_count, OBJ_SYNC, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_PARAMETER);
		return;
	}
	/* Fence values follow when the guest sent them (all but legacy). */
	if (m->cmd_size >= fences_offset + c->object_count * sizeof(u64))
		fences = (const u64 *)(m->cmd + fences_offset);

	if (c->context_count == 0) {
		struct d3dkmt_signalsynchronizationobjectfromcpu a = {0};

		if (!fences || !owns(h, c->device.v, OBJ_DEVICE, m->process)) {
			reply_ntstatus(m, DXG_STATUS_INVALID_PARAMETER);
			return;
		}
		a.device = c->device;
		a.object_count = c->object_count;
		a.objects = (u64)(uintptr_t)objects;
		a.fence_values = (u64)(uintptr_t)fences;
		a.flags = c->flags;
		status = h->kmt.SignalSynchronizationObjectFromCpu(&a);
	} else if (owns(h, contexts[0].v, OBJ_HWQUEUE, m->process)) {
		struct d3dkmt_submitsignalsyncobjectstohwqueue a = {0};

		if (!fences || !owns_all(h, contexts, c->context_count,
					 OBJ_HWQUEUE, m->process)) {
			reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
			return;
		}
		a.flags = c->flags;
		a.hwqueue_count = c->context_count;
		a.hwqueues = (u64)(uintptr_t)contexts;
		a.object_count = c->object_count;
		a.objects = (u64)(uintptr_t)objects;
		a.fence_values = (u64)(uintptr_t)fences;
		status = h->kmt.SubmitSignalSyncObjectsToHwQueue(&a);
	} else {
		struct d3dkmt_signalsynchronizationobjectfromgpu2 a = {0};

		if (!owns_all(h, contexts, c->context_count, OBJ_CONTEXT,
			      m->process)) {
			reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
			return;
		}
		a.object_count = c->object_count;
		a.objects = (u64)(uintptr_t)objects;
		a.flags = c->flags;
		a.context_count = c->context_count;
		a.contexts = (u64)(uintptr_t)contexts;
		if (fences)
			a.monitored_fence_values = (u64)(uintptr_t)fences;
		else
			a.fence_value = c->fence_value;
		status = h->kmt.SignalSynchronizationObjectFromGpu2(&a);
	}
	reply_ntstatus(m, status);
}

static void wait_reap(struct dxghost *h, bool all)
{
	struct dxghost_wait **link, *w;

	AcquireSRWLockExclusive(&h->wait_lock);
	link = &h->waits;
	while ((w = *link) != NULL) {
		if (!all && !InterlockedCompareExchange(&w->fired, 0, 0)) {
			link = &w->next;
			continue;
		}
		*link = w->next;
		ReleaseSRWLockExclusive(&h->wait_lock);
		if (all)
			SetThreadpoolWait(w->tp, NULL, NULL);
		WaitForThreadpoolWaitCallbacks(w->tp, all);
		CloseThreadpoolWait(w->tp);
		CloseHandle(w->event);
		free(w);
		AcquireSRWLockExclusive(&h->wait_lock);
		link = &h->waits;
	}
	ReleaseSRWLockExclusive(&h->wait_lock);
}

void dxghost_waits_close(struct dxghost *h)
{
	wait_reap(h, true);
}

static VOID CALLBACK wait_done(PTP_CALLBACK_INSTANCE instance, PVOID context,
			       PTP_WAIT tp, TP_WAIT_RESULT result)
{
	struct dxghost_wait *w = context;

	(void)instance;
	(void)tp;
	(void)result;
	if (!InterlockedCompareExchange(&w->h->closing, 0, 0))
		dxghost_signal_guest_event(w->h, w->process, w->guest_event);
	InterlockedExchange(&w->fired, 1);
}

static void cmd_wait_cpu(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_waitforsyncobjectfromcpu);
	struct d3dkmt_waitforsynchronizationobjectfromcpu a = {0};
	struct dxghost *h = m->h;
	uint64_t objects_size = (uint64_t)c->object_count * sizeof(u32);
	const struct d3dkmthandle *objects = tail(m, sizeof(*c), objects_size);
	const u64 *fences = tail(m, sizeof(*c) + objects_size,
				 (uint64_t)c->object_count * sizeof(u64));
	struct dxghost_wait *w;
	int32_t status;

	if (!objects || !fences)
		return;
	if (!c->object_count || c->object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
	    !owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_all(h, objects, c->object_count, OBJ_SYNC, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.device = c->device;
	a.object_count = c->object_count;
	a.objects = (u64)(uintptr_t)objects;
	a.fence_values = (u64)(uintptr_t)fences;
	a.flags = c->flags;
	if (!c->guest_event_pointer) {
		/* No guest event: block this process's lane until done. */
		reply_ntstatus(m, h->kmt.WaitForSynchronizationObjectFromCpu(&a));
		return;
	}

	wait_reap(h, false);
	w = calloc(1, sizeof(*w));
	if (!w) {
		reply_ntstatus(m, DXG_STATUS_NO_MEMORY);
		return;
	}
	w->h = h;
	w->process = m->process;
	w->guest_event = c->guest_event_pointer;
	w->event = CreateEventW(NULL, TRUE, FALSE, NULL);
	w->tp = w->event ? CreateThreadpoolWait(wait_done, w, NULL) : NULL;
	if (!w->tp) {
		if (w->event)
			CloseHandle(w->event);
		free(w);
		reply_ntstatus(m, DXG_STATUS_NO_MEMORY);
		return;
	}
	a.async_event = (u64)(uintptr_t)w->event;
	status = h->kmt.WaitForSynchronizationObjectFromCpu(&a);
	if (status < 0) {
		CloseThreadpoolWait(w->tp);
		CloseHandle(w->event);
		free(w);
		reply_ntstatus(m, status);
		return;
	}
	AcquireSRWLockExclusive(&h->wait_lock);
	w->next = h->waits;
	h->waits = w;
	ReleaseSRWLockExclusive(&h->wait_lock);
	SetThreadpoolWait(w->tp, w->event, NULL);
	reply_ntstatus(m, DXG_STATUS_SUCCESS);
}

static void cmd_wait_gpu(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_waitforsyncobjectfromgpu);
	struct dxghost *h = m->h;
	uint64_t fences_size = (uint64_t)c->object_count * sizeof(u64);
	const u64 *fences = tail(m, offsetof(
		struct dxgkvmb_command_waitforsyncobjectfromgpu, fence_values),
		fences_size);
	const struct d3dkmthandle *objects = tail(m, offsetof(
		struct dxgkvmb_command_waitforsyncobjectfromgpu, fence_values) +
		fences_size, (uint64_t)c->object_count * sizeof(u32));
	int32_t status;

	if (!fences || !objects)
		return;
	if (!c->object_count || c->object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
	    !owns_all(h, objects, c->object_count, OBJ_SYNC, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	if (owns(h, c->context.v, OBJ_HWQUEUE, m->process)) {
		struct d3dkmt_submitwaitforsyncobjectstohwqueue a = {0};

		a.hwqueue = c->context;
		a.object_count = c->object_count;
		a.objects = (u64)(uintptr_t)objects;
		a.fence_values = (u64)(uintptr_t)fences;
		status = h->kmt.SubmitWaitForSyncObjectsToHwQueue(&a);
	} else if (owns(h, c->context.v, OBJ_CONTEXT, m->process)) {
		struct d3dkmt_waitforsynchronizationobjectfromgpu a = {0};

		a.context = c->context;
		a.object_count = c->object_count;
		a.objects = (u64)(uintptr_t)objects;
		if (c->legacy_fence_object)
			a.fence_value = fences[0];
		else
			a.monitored_fence_values = (u64)(uintptr_t)fences;
		status = h->kmt.WaitForSynchronizationObjectFromGpu(&a);
	} else {
		status = DXG_STATUS_INVALID_HANDLE;
	}
	reply_ntstatus(m, status);
}

/* --- Queries and the rest -------------------------------------------- */

static void cmd_escape(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_escape);
	uint32_t size = c->priv_drv_data_size;
	const u8 *in = tail(m, offsetof(struct dxgkvmb_command_escape,
					priv_drv_data), size);
	struct d3dkmt_escape a = {0};
	struct dxghost *h = m->h;
	u8 *out;

	if (!in)
		return;
	out = reply(m, size);
	if (size && !out)
		return;
	if (!is_adapter(h, c->adapter.v) ||
	    !owns_optional(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_optional(h, c->context.v, OBJ_CONTEXT, m->process)) {
		fail_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	if (size)
		memcpy(out, in, size);
	a.adapter = c->adapter;
	a.device = c->device;
	a.type = c->type;
	a.flags = c->flags;
	a.priv_drv_data = size ? (u64)(uintptr_t)out : 0;
	a.priv_drv_data_size = size;
	a.context = c->context;
	fail_ntstatus(m, h->kmt.Escape(&a));
}

static void cmd_query_vidmem(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_queryvideomemoryinfo);
	struct dxgkvmb_command_queryvideomemoryinfo_return *r =
		reply(m, sizeof(*r));
	struct d3dkmt_queryvideomemoryinfo a = {0};
	struct dxghost *h = m->h;

	if (!r)
		return;
	if (!is_adapter(h, c->adapter.v)) {
		fail_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.adapter = c->adapter;
	a.memory_segment_group = c->memory_segment_group;
	a.physical_adapter_index = c->physical_adapter_index;
	fail_ntstatus(m, h->kmt.QueryVideoMemoryInfo(&a));
	r->budget = a.budget;
	r->current_usage = a.current_usage;
	r->current_reservation = a.current_reservation;
	r->available_for_reservation = a.available_for_reservation;
}

static void cmd_get_device_state(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_getdevicestate);
	struct dxgkvmb_command_getdevicestate_return *r = reply(m, sizeof(*r));
	struct dxghost *h = m->h;

	if (!r)
		return;
	r->args = c->args;
	if (!owns(h, c->args.device.v, OBJ_DEVICE, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	r->status.v = h->kmt.GetDeviceState(&r->args);
}

static void cmd_mark_device_error(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_markdeviceaserror);
	struct d3dkmt_markdeviceaserror a = c->args;
	struct dxghost *h = m->h;

	if (!owns(h, a.device.v, OBJ_DEVICE, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	reply_ntstatus(m, h->kmt.MarkDeviceAsError(&a));
}

static void cmd_query_clock(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_queryclockcalibration);
	struct dxgkvmb_command_queryclockcalibration_return *r =
		reply(m, sizeof(*r));
	struct d3dkmt_queryclockcalibration a = c->args;
	struct dxghost *h = m->h;

	if (!r)
		return;
	if (!is_adapter(h, a.adapter.v)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	r->status.v = h->kmt.QueryClockCalibration(&a);
	r->clock_data = a.clock_data;
}

static void cmd_flush_heap_transitions(struct dxghost_msg *m)
{
	struct d3dkmt_flushheaptransitions a = {0};
	struct dxghost *h = m->h;

	a.adapter.v = h->adapter;
	reply_ntstatus(m, h->kmt.FlushHeapTransitions ?
			  h->kmt.FlushHeapTransitions(&a) :
			  DXG_STATUS_SUCCESS);
}

static void cmd_query_residency(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_queryallocationresidency);
	struct d3dkmt_queryallocationresidency a = c->args;
	struct dxghost *h = m->h;
	uint32_t count = a.allocation_count;
	const struct d3dkmthandle *list =
		tail(m, sizeof(*c), (uint64_t)count * sizeof(u32));
	struct dxgkvmb_command_queryallocationresidency_return *r;
	uint32_t entries = count ? count : 1;

	if (!list)
		return;
	r = reply(m, (uint32_t)(sizeof(*r) + entries * sizeof(u32)));
	if (!r)
		return;
	if (!owns(h, a.device.v, OBJ_DEVICE, m->process) ||
	    !owns_optional(h, a.resource.v, OBJ_RESOURCE, m->process) ||
	    !owns_all(h, list, count, OBJ_ALLOCATION, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.allocations = count ? (u64)(uintptr_t)list : 0;
	a.residency_status = (u64)(uintptr_t)&r[1];
	r->status.v = h->kmt.QueryAllocationResidency(&a);
}

static void cmd_update_alloc_property(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_updateallocationproperty);
	struct dxgkvmb_command_updateallocationproperty_return *r =
		reply(m, sizeof(*r));
	struct d3dddi_updateallocproperty a = c->args;
	struct dxghost *h = m->h;

	if (!r)
		return;
	if (!owns(h, a.paging_queue.v, OBJ_PAGING_QUEUE, m->process) ||
	    !owns(h, a.allocation.v, OBJ_ALLOCATION, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	r->status.v = h->kmt.UpdateAllocationProperty(&a);
	r->paging_fence_value = a.paging_fence_value;
}

static void cmd_set_allocation_priority(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_setallocationpriority);
	struct d3dkmt_setallocationpriority a = {0};
	struct dxghost *h = m->h;
	uint32_t count = c->allocation_count;
	const struct d3dkmthandle *list =
		tail(m, sizeof(*c), (uint64_t)count * sizeof(u32));
	const u32 *priorities = tail(m, sizeof(*c) + (uint64_t)count * 4,
				     (uint64_t)(count ? count : 1) * 4);

	if (!list || !priorities)
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_optional(h, c->resource.v, OBJ_RESOURCE, m->process) ||
	    !owns_all(h, list, count, OBJ_ALLOCATION, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.device = c->device;
	a.resource = c->resource;
	a.allocation_list = count ? (u64)(uintptr_t)list : 0;
	a.allocation_count = count;
	a.priorities = (u64)(uintptr_t)priorities;
	reply_ntstatus(m, h->kmt.SetAllocationPriority(&a));
}

static void cmd_get_allocation_priority(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_getallocationpriority);
	struct d3dkmt_getallocationpriority a = {0};
	struct dxgkvmb_command_getallocationpriority_return *r;
	struct dxghost *h = m->h;
	uint32_t count = c->allocation_count;
	const struct d3dkmthandle *list =
		tail(m, sizeof(*c), (uint64_t)count * sizeof(u32));

	if (!list)
		return;
	r = reply(m, (uint32_t)(sizeof(*r) + (count ? count : 1) * 4));
	if (!r)
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_optional(h, c->resource.v, OBJ_RESOURCE, m->process) ||
	    !owns_all(h, list, count, OBJ_ALLOCATION, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.device = c->device;
	a.resource = c->resource;
	a.allocation_list = count ? (u64)(uintptr_t)list : 0;
	a.allocation_count = count;
	a.priorities = (u64)(uintptr_t)&r[1];
	r->status.v = h->kmt.GetAllocationPriority(&a);
}

static void cmd_set_context_priority(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_setcontextschedulingpriority2);
	struct d3dkmt_setcontextschedulingpriority a = {0};
	struct dxghost *h = m->h;

	if (!owns(h, c->context.v, OBJ_CONTEXT, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.context = c->context;
	a.priority = c->priority;
	reply_ntstatus(m, c->in_process ?
			  h->kmt.SetContextInProcessSchedulingPriority(&a) :
			  h->kmt.SetContextSchedulingPriority(&a));
}

static void cmd_get_context_priority(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_getcontextschedulingpriority);
	struct dxgkvmb_command_getcontextschedulingpriority_return *r =
		reply(m, sizeof(*r));
	struct d3dkmt_getcontextschedulingpriority a = {0};
	struct dxghost *h = m->h;

	if (!r)
		return;
	if (!owns(h, c->context.v, OBJ_CONTEXT, m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.context = c->context;
	r->status.v = c->in_process ?
		      h->kmt.GetContextInProcessSchedulingPriority(&a) :
		      h->kmt.GetContextSchedulingPriority(&a);
	r->priority = a.priority;
}

static void cmd_offer(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_offerallocations);
	struct d3dkmt_offerallocations a = {0};
	struct dxghost *h = m->h;
	const struct d3dkmthandle *list = c->allocations;

	if (!tail(m, offsetof(struct dxgkvmb_command_offerallocations,
			      allocations),
		  (uint64_t)c->allocation_count * sizeof(*list)))
		return;
	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns_all(h, list, c->allocation_count,
		      c->resources ? OBJ_RESOURCE : OBJ_ALLOCATION,
		      m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.device = c->device;
	if (c->resources)
		a.resources = (u64)(uintptr_t)list;
	else
		a.allocations = (u64)(uintptr_t)list;
	a.allocation_count = c->allocation_count;
	a.priority = c->priority;
	a.flags = c->flags;
	reply_ntstatus(m, h->kmt.OfferAllocations(&a));
}

static void cmd_reclaim(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_reclaimallocations);
	struct d3dkmt_reclaimallocations2 a = {0};
	struct dxgkvmb_command_reclaimallocations_return *r;
	struct dxghost *h = m->h;
	const struct d3dkmthandle *list = c->allocations;
	uint32_t count = c->allocation_count;

	if (!tail(m, offsetof(struct dxgkvmb_command_reclaimallocations,
			      allocations), (uint64_t)count * sizeof(*list)))
		return;
	r = reply(m, (uint32_t)(sizeof(*r) + (c->write_results && count ?
					      (count - 1) * 4 : 0)));
	if (!r)
		return;
	if (!owns(h, c->paging_queue.v, OBJ_PAGING_QUEUE, m->process) ||
	    !owns_all(h, list, count,
		      c->resources ? OBJ_RESOURCE : OBJ_ALLOCATION,
		      m->process)) {
		r->status.v = DXG_STATUS_INVALID_HANDLE;
		return;
	}
	a.paging_queue = c->paging_queue;
	a.allocation_count = count;
	if (c->resources)
		a.resources = (u64)(uintptr_t)list;
	else
		a.allocations = (u64)(uintptr_t)list;
	a.results = c->write_results ? (u64)(uintptr_t)r->discarded : 0;
	r->status.v = h->kmt.ReclaimAllocations2(&a);
	r->paging_fence_value = a.paging_fence_value;
}

static void cmd_change_reservation(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_changevideomemoryreservation);
	struct d3dkmt_changevideomemoryreservation a = c->args;
	struct dxghost *h = m->h;

	/* Always this process: the guest may not name host processes. */
	a.process = 0;
	a.adapter.v = h->adapter;
	reply_ntstatus(m, h->kmt.ChangeVideoMemoryReservation(&a));
}

static void cmd_invalidate_cache(struct dxghost_msg *m)
{
	COMMAND(dxgkvmb_command_invalidatecache);
	struct d3dkmt_invalidatecache a = {0};
	struct dxghost *h = m->h;

	if (!owns(h, c->device.v, OBJ_DEVICE, m->process) ||
	    !owns(h, c->allocation.v, OBJ_ALLOCATION, m->process)) {
		reply_ntstatus(m, DXG_STATUS_INVALID_HANDLE);
		return;
	}
	a.device = c->device;
	a.allocation = c->allocation;
	a.offset = c->offset;
	a.length = c->length;
	reply_ntstatus(m, h->kmt.InvalidateCache ?
			  h->kmt.InvalidateCache(&a) : DXG_STATUS_SUCCESS);
}

/* Replies for commands that are recognised but not offered. */
static void not_supported(struct dxghost_msg *m)
{
	if (!m->res_cap)
		return;
	reply_ntstatus(m, DXG_STATUS_NOT_SUPPORTED);
	m->res_len = m->res_cap;
}

void dxghost_exec_vgpu(struct dxghost_msg *m, uint32_t type)
{
	struct dxghost *h = m->h;

	/* Missing entry points (older Windows) and process-less requests. */
	switch (type) {
	case DXGK_VMBCOMMAND_OPENADAPTER:
		cmd_open_adapter(m);
		return;
	case DXGK_VMBCOMMAND_CLOSEADAPTER:
		cmd_close_adapter(m);
		return;
	case DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO:
		cmd_internal_adapter_info(m);
		return;
	case DXGK_VMBCOMMAND_QUERYADAPTERINFO:
		cmd_query_adapter_info(m);
		return;
	case DXGK_VMBCOMMAND_ISFEATUREENABLED:
		cmd_feature(m);
		return;
	default:
		break;
	}
	if (!m->process || !h->adapter) {
		m->status = VIRTIO_DXG_ERR_INVALID;
		return;
	}

	switch (type) {
	case DXGK_VMBCOMMAND_CREATEDEVICE:
		cmd_create_device(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYDEVICE:
		cmd_destroy_device(m);
		break;
	case DXGK_VMBCOMMAND_FLUSHDEVICE:
		reply_ntstatus(m, DXG_STATUS_SUCCESS);
		break;
	case DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL:
		cmd_create_context(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYCONTEXT:
		cmd_destroy_context(m);
		break;
	case DXGK_VMBCOMMAND_CREATEPAGINGQUEUE:
		cmd_create_paging_queue(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE:
		cmd_destroy_paging_queue(m);
		break;
	case DXGK_VMBCOMMAND_CREATEALLOCATION:
		cmd_create_allocation(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYALLOCATION:
		cmd_destroy_allocation(m);
		break;
	case DXGK_VMBCOMMAND_LOCK2:
		cmd_lock2(m);
		break;
	case DXGK_VMBCOMMAND_UNLOCK2:
		cmd_unlock2(m);
		break;
	case DXGK_VMBCOMMAND_MAKERESIDENT:
		cmd_make_resident(m);
		break;
	case DXGK_VMBCOMMAND_EVICT:
		cmd_evict(m);
		break;
	case DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS:
		cmd_reserve_gpu_va(m);
		break;
	case DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS:
		cmd_free_gpu_va(m);
		break;
	case DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS:
		cmd_map_gpu_va(m);
		break;
	case DXGK_VMBCOMMAND_UPDATEGPUVIRTUALADDRESS:
		cmd_update_gpu_va(m);
		break;
	case DXGK_VMBCOMMAND_SUBMITCOMMAND:
		cmd_submit(m);
		break;
	case DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE:
		cmd_submit_hwqueue(m);
		break;
	case DXGK_VMBCOMMAND_CREATEHWQUEUE:
		cmd_create_hwqueue(m);
		break;
	case DXGK_VMBCOMMAND_DESTROYHWQUEUE:
		cmd_destroy_hwqueue(m);
		break;
	case DXGK_VMBCOMMAND_CREATESYNCOBJECT:
		cmd_create_sync(m);
		break;
	case DXGK_VMBCOMMAND_SIGNALSYNCOBJECT:
		cmd_signal_sync(m);
		break;
	case DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU:
		cmd_wait_cpu(m);
		break;
	case DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU:
		cmd_wait_gpu(m);
		break;
	case DXGK_VMBCOMMAND_ESCAPE:
		cmd_escape(m);
		break;
	case dxgk_vmbcommand_queryvideomemoryinfo:
		cmd_query_vidmem(m);
		break;
	case DXGK_VMBCOMMAND_GETDEVICESTATE:
		cmd_get_device_state(m);
		break;
	case DXGK_VMBCOMMAND_MARKDEVICEASERROR:
		cmd_mark_device_error(m);
		break;
	case DXGK_VMBCOMMAND_QUERYCLOCKCALIBRATION:
		cmd_query_clock(m);
		break;
	case DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS:
		cmd_flush_heap_transitions(m);
		break;
	case DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY:
		cmd_query_residency(m);
		break;
	case DXGK_VMBCOMMAND_UPDATEALLOCATIONPROPERTY:
		cmd_update_alloc_property(m);
		break;
	case DXGK_VMBCOMMAND_SETALLOCATIONPRIORITY:
		cmd_set_allocation_priority(m);
		break;
	case DXGK_VMBCOMMAND_GETALLOCATIONPRIORITY:
		cmd_get_allocation_priority(m);
		break;
	case DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY:
		cmd_set_context_priority(m);
		break;
	case DXGK_VMBCOMMAND_GETCONTEXTSCHEDULINGPRIORITY:
		cmd_get_context_priority(m);
		break;
	case DXGK_VMBCOMMAND_OFFERALLOCATIONS:
		cmd_offer(m);
		break;
	case DXGK_VMBCOMMAND_RECLAIMALLOCATIONS:
		cmd_reclaim(m);
		break;
	case DXGK_VMBCOMMAND_CHANGEVIDEOMEMORYRESERVATION:
		cmd_change_reservation(m);
		break;
	case DXGK_VMBCOMMAND_INVALIDATECACHE:
		cmd_invalidate_cache(m);
		break;
	case DXGK_VMBCOMMAND_OPENRESOURCE:
	case DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA:
	case DXGK_VMBCOMMAND_QUERYSTATISTICS:
	case DXGK_VMBCOMMAND_SETEXISTINGSYSMEMPAGES:
	case DXGK_VMBCOMMAND_SETEXISTINGSYSMEMSTORE:
		not_supported(m);
		break;
	default:
		m->status = VIRTIO_DXG_ERR_UNSUPPORTED;
		break;
	}
}
