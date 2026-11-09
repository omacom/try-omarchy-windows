// SPDX-License-Identifier: GPL-2.0
/*
 * dxghost lifecycle, request framing and dispatch.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

void dxghost_logf(struct dxghost *h, const char *format, ...)
{
	char line[512];
	va_list ap;

	if (!h->vmm.log)
		return;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	h->vmm.log(h->vmm.opaque, line);
	{
		/* Development: DXGHOST_LOG names a file that gets every line,
		 * flushed, so nothing is lost when the VMM is killed. */
		const char *path = getenv("DXGHOST_LOG");
		FILE *f = path ? fopen(path, "a") : NULL;

		if (f) {
			fprintf(f, "%s\n", line);
			fclose(f);
		}
	}
}

void dxghost_signal_guest_event(struct dxghost *h, uint32_t process,
				uint64_t guest_event)
{
	struct dxgkvmb_command_signalguestevent packet;

	memset(&packet, 0, sizeof(packet));
	packet.hdr.process.v = process;
	packet.hdr.channel_type = DXGKVMB_HOST_TO_VM;
	packet.hdr.command_type = DXGK_VMBCOMMAND_SIGNALGUESTEVENT;
	packet.event = guest_event;
	h->vmm.event(h->vmm.opaque, &packet, sizeof(packet));
}

/* Picks the adapter to expose: the configured LUID, else the first NVIDIA. */
static int find_adapter(struct dxghost *h)
{
	struct d3dkmt_adapterinfo adapters[16];
	struct {
		u32 num_adapters;
		u32 reserved;
		u64 adapters;
	} enumerate = {16, 0, (u64)(uintptr_t)adapters};
	uint32_t i;
	int found = -1;

	if (h->config.adapter_luid_low || h->config.adapter_luid_high) {
		h->luid.LowPart = h->config.adapter_luid_low;
		h->luid.HighPart = h->config.adapter_luid_high;
		return 0;
	}
	if (!h->kmt.EnumAdapters2 || h->kmt.EnumAdapters2(&enumerate) < 0)
		return -1;
	for (i = 0; i < enumerate.num_adapters && i < 16; i++) {
		struct dxghost_device_ids ids = {0};
		struct d3dkmt_queryadapterinfo query = {0};
		struct d3dkmt_closeadapter close = {0};

		query.adapter = adapters[i].adapter_handle;
		query.type = DXGHOST_QAI_PHYSICALADAPTERDEVICEIDS;
		query.private_data = (u64)(uintptr_t)&ids;
		query.private_data_size = sizeof(ids);
		if (found < 0 && h->kmt.QueryAdapterInfo(&query) >= 0 &&
		    ids.vendor_id == 0x10de) {
			h->luid.LowPart = adapters[i].adapter_luid.a;
			h->luid.HighPart = (LONG)adapters[i].adapter_luid.b;
			found = 0;
		}
		close.adapter_handle = adapters[i].adapter_handle;
		h->kmt.CloseAdapter(&close);
	}
	return found;
}

int dxghost_create(const struct dxghost_config *config,
		   const struct dxghost_vmm *vmm, struct dxghost **out)
{
	struct dxghost *h;

	*out = NULL;
	if (!vmm || !vmm->map || !vmm->unmap || !vmm->event)
		return -1;
	h = calloc(1, sizeof(*h));
	if (!h)
		return -1;
	h->config = *config;
	h->vmm = *vmm;
	InitializeSRWLock(&h->lock);
	InitializeSRWLock(&h->map_lock);
	InitializeSRWLock(&h->wait_lock);
	h->next_proc = 1;
	if (config->kmt)
		h->kmt = *config->kmt;
	else if (dxghost_kmt_load(&h->kmt))
		goto fail;
	if (!h->kmt.OpenAdapterFromLuid || !h->kmt.CreateDevice) {
		dxghost_logf(h, "D3DKMT is unavailable");
		goto fail;
	}
	if (find_adapter(h)) {
		dxghost_logf(h, "no adapter LUID configured");
		goto fail;
	}
	/* Development: DXGHOST_TRACE logs every command and its result. */
	h->trace = getenv("DXGHOST_TRACE") != NULL;
	h->stats = getenv("DXGHOST_STATS") != NULL;
	/* Diagnostics: DXGHOST_NO_INLINE_LOCK2 / _UNLOCK2 send them to lanes. */
	h->inline_lock2 = getenv("DXGHOST_NO_INLINE_LOCK2") == NULL;
	h->inline_unlock2 = getenv("DXGHOST_NO_INLINE_UNLOCK2") == NULL;
	{
		const char *mb = getenv("DXGHOST_LOCK_CACHE_MB");

		h->lock_cache_max = (mb ? strtoull(mb, NULL, 10) : 1024) << 20;
	}
	{
		LARGE_INTEGER f, now;

		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&now);
		h->qpf = f.QuadPart;
		h->stats_due = now.QuadPart + 10 * f.QuadPart;
	}
	dxghost_logf(h, "host GPU ready");
	*out = h;
	return 0;
fail:
	free(h);
	return -1;
}

void dxghost_reset(struct dxghost *h)
{
	uint32_t i;

	dxghost_lock(h);
	for (i = 0; i < h->proc_count; i++) {
		if (!h->procs[i].live)
			continue;
		dxghost_unlock(h);
		dxghost_destroy_process_objects(h, h->procs[i].token);
		dxghost_lock(h);
		h->procs[i].live = false;
	}
	h->proc_count = 0;
	if (h->adapter && h->kmt.CloseAdapter) {
		struct d3dkmt_closeadapter close = {0};

		close.adapter_handle.v = h->adapter;
		h->kmt.CloseAdapter(&close);
	}
	h->adapter = 0;
	h->adapter_refs = 0;
	while (h->fence_page_count) {
		struct dxghost_fence_page *p = &h->fence_pages[0];

		p->refs = 1;
		dxghost_fence_unmap(h, p->gpa);
	}
	dxghost_objtab_free(&h->objects);
	dxghost_io_reset(h, 0, 0);
	dxghost_unlock(h);
}

void dxghost_destroy(struct dxghost *h)
{
	if (!h)
		return;
	InterlockedExchange(&h->closing, 1);
	dxghost_waits_close(h);
	dxghost_reset(h);
	free(h->vas);
	free(h->io_free);
	free(h->fence_pages);
	free(h->deferred);
	free(h->procs);
	free(h);
}

void dxghost_device_config(struct dxghost *h, struct virtio_dxg_config *c)
{
	memset(c, 0, sizeof(*c));
	c->vmbus_version = DXGHOST_INTERFACE_VERSION;
	c->host_caps = 0;	/* guest page mapping: not yet */
	c->flags = 0;
	c->vgpu_luid_low = h->luid.LowPart;
	c->vgpu_luid_high = (uint32_t)h->luid.HighPart;
	memcpy(c->adapter_guid, h->config.adapter_guid,
	       sizeof(c->adapter_guid));
	c->max_message = VIRTIO_DXG_MAX_MESSAGE;
	c->event_size = VIRTIO_DXG_EVENT_SIZE;
}

/* Locates the dxgkvmb command inside a message with its extended header. */
static const uint8_t *command_of(const void *message, uint32_t size,
				 uint32_t *cmd_size)
{
	const struct dxgvmb_ext_header *ext = message;

	if (size < sizeof(*ext) + sizeof(struct dxgkvmb_command_vgpu_to_host))
		return NULL;
	if (ext->command_offset < sizeof(*ext) ||
	    ext->command_offset > size - sizeof(struct dxgkvmb_command_vgpu_to_host))
		return NULL;
	*cmd_size = size - ext->command_offset;
	return (const uint8_t *)message + ext->command_offset;
}

uint32_t dxghost_lane(const void *message, uint32_t size)
{
	uint32_t cmd_size;
	const uint8_t *cmd = command_of(message, size, &cmd_size);

	if (!cmd)
		return 0;
	return ((const struct dxgkvmb_command_vgpu_to_host *)cmd)->process.v;
}

bool dxghost_nonblocking(const void *message, uint32_t size)
{
	uint32_t cmd_size;
	const uint8_t *cmd = command_of(message, size, &cmd_size);
	const struct dxgkvmb_command_vgpu_to_host *hdr = (const void *)cmd;

	if (!cmd || hdr->channel_type != DXGKVMB_VGPU_TO_HOST)
		return false;
	switch (hdr->command_type) {
	case DXGK_VMBCOMMAND_SIGNALSYNCOBJECT:
	case DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU:
	case DXGK_VMBCOMMAND_SUBMITCOMMAND:
	case DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE:
	case DXGK_VMBCOMMAND_GETDEVICESTATE:
		return true;
	case DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU:
		/* Without a guest event the host call blocks until done. */
		return cmd_size >= sizeof(struct dxgkvmb_command_waitforsyncobjectfromcpu) &&
		       ((const struct dxgkvmb_command_waitforsyncobjectfromcpu *)
			cmd)->guest_event_pointer != 0;
	default:
		return false;
	}
}

static bool process_live(struct dxghost *h, uint32_t token)
{
	uint32_t i;
	bool live = false;

	AcquireSRWLockShared(&h->lock);
	for (i = 0; i < h->proc_count; i++) {
		if (h->procs[i].token == token && h->procs[i].live) {
			live = true;
			break;
		}
	}
	ReleaseSRWLockShared(&h->lock);
	return live;
}

static void stats_dump(struct dxghost *h, LONG64 now)
{
	LONG64 due = h->stats_due;
	unsigned c, t;

	/* One thread writes each table. */
	if (now < due || InterlockedCompareExchange64(&h->stats_due,
			now + 10 * h->qpf, due) != due)
		return;
	for (c = 0; c < 2; c++) {
		for (t = 0; t < DXGHOST_STAT_TYPES; t++) {
			struct dxghost_stat *s = &h->stat[c][t];
			LONG64 n = InterlockedExchange64(&s->count, 0);
			LONG64 ticks = InterlockedExchange64(&s->ticks, 0);
			LONG64 max = InterlockedExchange64(&s->max, 0);

			if (!n)
				continue;
			dxghost_logf(h, "stats %s %4u: %8lld calls, avg %7.1f us, "
				     "max %8.1f us, total %8.1f ms",
				     c ? "adapter" : "global ", c ? t : t + 1000,
				     (long long)n,
				     ticks * 1e6 / h->qpf / n, max * 1e6 / h->qpf,
				     ticks * 1e3 / h->qpf);
		}
	}
}

static void stats_add(struct dxghost *h, unsigned channel, uint32_t type,
		      LONG64 start)
{
	LARGE_INTEGER end;
	struct dxghost_stat *s;
	LONG64 ticks, max;

	QueryPerformanceCounter(&end);
	ticks = end.QuadPart - start;
	if (channel == 0 && type >= DXGK_VMBCOMMAND_VM_TO_HOST_FIRST)
		type -= DXGK_VMBCOMMAND_VM_TO_HOST_FIRST;	/* 1000 + n */
	s = &h->stat[channel][type < DXGHOST_STAT_TYPES ? type :
			      DXGHOST_STAT_TYPES - 1];
	InterlockedIncrement64(&s->count);
	InterlockedAdd64(&s->ticks, ticks);
	while ((max = s->max) < ticks &&
	       InterlockedCompareExchange64(&s->max, ticks, max) != max)
		;
	stats_dump(h, end.QuadPart);
}

static bool execute(struct dxghost *h, uint32_t channel,
		    const void *message, uint32_t size, void *result,
		    uint32_t result_capacity, struct virtio_dxg_resp *resp,
		    bool inline_call);

void dxghost_execute(struct dxghost *h, uint32_t channel,
		     const void *message, uint32_t size, void *result,
		     uint32_t result_capacity, struct virtio_dxg_resp *resp)
{
	execute(h, channel, message, size, result, result_capacity, resp, false);
}

/* Whether a Lock2 can be answered from a view mapped already. */
static bool lock2_view_mapped(struct dxghost *h, const uint8_t *cmd,
			      uint32_t cmd_size)
{
	const struct dxgkvmb_command_lock2 *c = (const void *)cmd;
	const struct dxghost_obj *o;
	bool mapped;

	if (cmd_size < sizeof(*c))
		return false;
	AcquireSRWLockShared(&h->lock);
	o = dxghost_obj_find(h, c->args.allocation.v);
	mapped = o && o->type == OBJ_ALLOCATION && o->gpa;
	ReleaseSRWLockShared(&h->lock);
	return mapped;
}

bool dxghost_try_inline(struct dxghost *h, uint32_t channel,
			const void *message, uint32_t size, void *result,
			uint32_t result_capacity, struct virtio_dxg_resp *resp)
{
	uint32_t cmd_size;
	const uint8_t *cmd = command_of(message, size, &cmd_size);
	const struct dxgkvmb_command_vgpu_to_host *hdr = (const void *)cmd;

	if (!dxghost_nonblocking(message, size)) {
		if (!cmd || hdr->channel_type != DXGKVMB_VGPU_TO_HOST)
			return false;
		if (!(hdr->command_type == DXGK_VMBCOMMAND_UNLOCK2 &&
		      h->inline_unlock2) &&
		    !(hdr->command_type == DXGK_VMBCOMMAND_LOCK2 &&
		      h->inline_lock2 && lock2_view_mapped(h, cmd, cmd_size)))
			return false;
	}
	return execute(h, channel, message, size, result, result_capacity,
		       resp, true);
}

static bool execute(struct dxghost *h, uint32_t channel,
		    const void *message, uint32_t size, void *result,
		    uint32_t result_capacity, struct virtio_dxg_resp *resp,
		    bool inline_call)
{
	LARGE_INTEGER start = {0};
	struct dxghost_msg m;
	const struct dxgkvmb_command_vgpu_to_host *vgpu;
	uint32_t channel_type, type;

	memset(resp, 0, sizeof(*resp));
	memset(&m, 0, sizeof(m));
	if (h->stats)
		QueryPerformanceCounter(&start);
	m.h = h;
	m.inline_call = inline_call;
	m.res = result;
	m.res_cap = result ? result_capacity : 0;
	m.cmd = command_of(message, size, &m.cmd_size);
	if (!m.cmd || (channel != VIRTIO_DXG_CHANNEL_GLOBAL &&
		       channel != VIRTIO_DXG_CHANNEL_ADAPTER)) {
		dxghost_logf(h, "malformed request: %u bytes on channel %u",
			     size, channel);
		resp->status = VIRTIO_DXG_ERR_INVALID;
		return true;
	}
	if (m.res_cap)
		memset(m.res, 0, m.res_cap);

	/*
	 * Both header kinds keep the channel type in the low byte at offset
	 * 12. With async messages enabled, adapter commands travel on the
	 * global channel, so the header decides, not the virtqueue.
	 */
	vgpu = (const void *)m.cmd;
	memcpy(&channel_type, m.cmd + 12, sizeof(channel_type));
	channel_type &= 0xff;
	type = vgpu->command_type;
	m.process = vgpu->process.v;

	if (channel_type == DXGKVMB_VM_TO_HOST) {
		if (type != DXGK_VMBCOMMAND_CREATEPROCESS &&
		    type != DXGK_VMBCOMMAND_SETIOSPACEREGION &&
		    type != DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT &&
		    type != DXGK_VMBCOMMAND_ISFEATUREENABLED_GLOBAL &&
		    !process_live(h, m.process)) {
			dxghost_logf(h, "global command %u from unknown "
				     "process 0x%x", type, m.process);
			resp->status = VIRTIO_DXG_ERR_INVALID;
			return true;
		}
		dxghost_exec_global(&m, type);
	} else if (channel_type == DXGKVMB_VGPU_TO_HOST) {
		if (m.process && !process_live(h, m.process)) {
			dxghost_logf(h, "adapter command %u from unknown "
				     "process 0x%x", type, m.process);
			resp->status = VIRTIO_DXG_ERR_INVALID;
			return true;
		}
		dxghost_exec_vgpu(&m, type);
	} else {
		dxghost_logf(h, "command %u on unknown channel type %u", type,
			     channel_type);
		m.status = VIRTIO_DXG_ERR_INVALID;
	}
	if (m.to_lane)
		return false;	/* undone; the lane runs it */
	resp->status = m.status;
	resp->size = m.res_len;
	resp->ntstatus = (uint32_t)m.ntstatus;
	if (h->stats)
		stats_add(h, channel_type == DXGKVMB_VM_TO_HOST ? 0 : 1, type,
			  start.QuadPart);
	if (h->trace) {
		uint32_t first = 0;

		if (m.res_len >= 4)
			memcpy(&first, m.res, 4);
		dxghost_logf(h, "%s %u: status %u ntstatus 0x%08x reply %u "
			     "bytes, first word 0x%08x",
			     channel_type == DXGKVMB_VM_TO_HOST ? "global" :
			     "adapter", type, m.status, (unsigned)m.ntstatus,
			     m.res_len, first);
	}
	return true;
}
