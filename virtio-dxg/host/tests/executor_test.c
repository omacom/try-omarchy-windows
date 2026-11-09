// SPDX-License-Identifier: GPL-2.0
/*
 * Runs the executor against a fake D3DKMT driver with messages built the
 * way the guest's dxgvmbus.c builds them. No GPU is touched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/host.h"

static int failures;
#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n",	\
				__FILE__, __LINE__, #cond);		\
			failures++;					\
		}							\
	} while (0)

/* --- Fake driver ------------------------------------------------------ */

#define NV_LUID 0x11ec6u
#define INTEL_LUID 0x134fau

static uint32_t next_handle = 0x40000100;
static uint32_t calls_destroy_device, calls_lock2, calls_unlock2;
static uint32_t calls_kmt_total;
static uint8_t *fence_page;
static uint32_t fence_slot;

static uint32_t new_handle(void) { return next_handle += 0x40; }

static int32_t __stdcall f_enum(void *p)
{
	struct { u32 n, r; u64 a; } *e = p;
	struct d3dkmt_adapterinfo *a = (void *)(uintptr_t)e->a;

	a[0].adapter_handle.v = 0x101;
	a[0].adapter_luid.a = INTEL_LUID;
	a[1].adapter_handle.v = 0x102;
	a[1].adapter_luid.a = NV_LUID;
	e->n = 2;
	return 0;
}

static int32_t __stdcall f_query(void *p)
{
	struct d3dkmt_queryadapterinfo *q = p;
	void *data = (void *)(uintptr_t)q->private_data;

	calls_kmt_total++;
	if (q->type == DXGHOST_QAI_PHYSICALADAPTERDEVICEIDS) {
		struct dxghost_device_ids *ids = data;

		ids->vendor_id = q->adapter.v == 0x102 ? 0x10de : 0x8086;
		return 0;
	}
	if (q->type == DXGHOST_QAI_UMDRIVERNAME) {
		const wchar_t *path = L"C:\\Windows\\System32\\DriverStore\\"
				      L"FileRepository\\nvami.inf_amd64_x\\"
				      L"nvwgf2umx.dll";
		u16 *out = (u16 *)((u8 *)data + 4);
		size_t i;

		for (i = 0; path[i]; i++)
			out[i] = (u16)path[i];
		return 0;
	}
	if (q->type == DXGHOST_QAI_WDDM_2_7_CAPS) {
		*(u32 *)data = 3;	/* supported + enabled */
		return 0;
	}
	if (q->type == DXGHOST_QAI_DRIVERVERSION) {
		*(u32 *)data = 3200;
		return 0;
	}
	return (int32_t)0xC00000BB;
}

static int32_t __stdcall f_close_adapter(void *p) { (void)p; return 0; }

static int32_t __stdcall f_open_luid(void *p)
{
	struct d3dkmt_openadapterfromluid *o = p;

	o->adapter_handle.v = o->adapter_luid.a == NV_LUID ? 0x500 : 0;
	return o->adapter_handle.v ? 0 : (int32_t)0xC000000D;
}

static int32_t __stdcall f_create_device(void *p)
{
	struct d3dkmt_createdevice *d = p;

	calls_kmt_total++;
	if (d->adapter.v != 0x500)
		return (int32_t)0xC0000008;
	d->device.v = new_handle();
	return 0;
}

static uint32_t calls_free_va;
static uint64_t freed_va_base, freed_va_size;

static int32_t __stdcall f_reserve_va(void *p)
{
	struct d3dddi_reservegpuvirtualaddress *a = p;

	a->virtual_address = a->base_address ? a->base_address : 0x200000000ull;
	return 0;
}

static int32_t __stdcall f_map_va(void *p)
{
	struct d3dddi_mapgpuvirtualaddress *a = p;

	if (!a->base_address)
		a->virtual_address = 0x300000000ull;	/* the driver picks */
	return 0;
}

static int32_t __stdcall f_free_va(void *p)
{
	const struct d3dkmt_freegpuvirtualaddress *a = p;

	calls_free_va++;
	freed_va_base = a->base_address;
	freed_va_size = a->size;
	return 0;
}

static int32_t __stdcall f_destroy_device(void *p)
{
	(void)p;
	calls_destroy_device++;
	calls_kmt_total++;
	return 0;
}

static int32_t __stdcall f_create_allocation(void *p)
{
	struct d3dkmt_createallocation *a = p;
	struct d3dddi_allocationinfo2 *info =
		(void *)(uintptr_t)a->allocation_info;
	uint32_t i;

	calls_kmt_total++;
	if (a->flags.create_resource)
		a->resource.v = new_handle();
	for (i = 0; i < a->alloc_count; i++) {
		info[i].allocation.v = new_handle();
		/* The KMD writes back into per-allocation private data. */
		if (info[i].priv_drv_data_size)
			*(u8 *)(uintptr_t)info[i].priv_drv_data ^= 0xff;
	}
	return 0;
}

static int32_t __stdcall f_destroy_allocation(void *p) { (void)p; return 0; }

static void *lock_memory;
static int32_t __stdcall f_lock2(void *p)
{
	struct d3dkmt_lock2 *l = p;

	calls_lock2++;
	if (!lock_memory)
		lock_memory = VirtualAlloc(NULL, 3 * 4096,
					   MEM_COMMIT | MEM_RESERVE,
					   PAGE_READWRITE | PAGE_WRITECOMBINE);
	l->data = (u64)(uintptr_t)lock_memory;
	return 0;
}

static int32_t __stdcall f_unlock2(void *p) { (void)p; calls_unlock2++; return 0; }

static int32_t __stdcall f_create_sync(void *p)
{
	struct d3dkmt_createsynchronizationobject2 *s = p;

	s->sync_object.v = new_handle();
	if (s->info.type == _D3DDDI_MONITORED_FENCE) {
		/* Several fences share one host page. */
		s->info.monitored_fence.fence_cpu_virtual_address =
			(void *)(fence_page + 0x10 + 8 * fence_slot++);
		s->info.monitored_fence.fence_gpu_virtual_address = 0xabc000;
	}
	return 0;
}

static int32_t __stdcall f_destroy_sync(void *p) { (void)p; return 0; }

static int32_t __stdcall f_wait_cpu(void *p)
{
	struct d3dkmt_waitforsynchronizationobjectfromcpu *w = p;

	/* Already satisfied: the driver sets the event at once. */
	if (w->async_event)
		SetEvent((HANDLE)(uintptr_t)w->async_event);
	return 0;
}

static int32_t __stdcall f_escape(void *p)
{
	struct d3dkmt_escape *e = p;

	if (e->priv_drv_data_size)
		*(u8 *)(uintptr_t)e->priv_drv_data = 0x5a;
	return (int32_t)0xC000000D;
}

/* --- Fake VMM --------------------------------------------------------- */

static uint32_t maps, unmaps;
static uint64_t last_map_gpa, last_map_size;
static volatile LONG events;
static uint64_t last_event;

/*
 * The VMM may hold its own lock while calling in (inline requests on vCPU
 * threads), so dxghost must never call it back with h->lock held: that
 * deadlocked QEMU. Map and unmap run on the test's thread, where a failed
 * try-lock can only mean dxghost holds it.
 */
static struct dxghost *test_h;
static int lock_violations;

static void check_unlocked(void)
{
	if (!test_h)
		return;
	if (TryAcquireSRWLockExclusive(&test_h->lock))
		ReleaseSRWLockExclusive(&test_h->lock);
	else
		lock_violations++;
}

static int vmm_map(void *o, uint64_t gpa, void *hva, uint64_t size, bool w)
{
	(void)o; (void)hva; (void)w;
	check_unlocked();
	maps++;
	last_map_gpa = gpa;
	last_map_size = size;
	return 0;
}

static void vmm_unmap(void *o, uint64_t gpa, uint64_t size)
{
	(void)o; (void)gpa; (void)size;
	check_unlocked();
	unmaps++;
}

static void vmm_event(void *o, const void *packet, uint32_t size)
{
	const struct dxgkvmb_command_signalguestevent *e = packet;

	(void)o;
	if (size == sizeof(*e) &&
	    e->hdr.command_type == DXGK_VMBCOMMAND_SIGNALGUESTEVENT)
		last_event = e->event;
	InterlockedIncrement(&events);
}

/* --- Messages, built like dxgvmbus.c ------------------------------------ */

static uint8_t msg_buf[8192];
static uint8_t res_buf[8192];

static void *msg(uint32_t size)
{
	struct dxgvmb_ext_header *ext = (void *)msg_buf;

	memset(msg_buf, 0, sizeof(*ext) + size);
	ext->command_offset = sizeof(*ext);
	return msg_buf + sizeof(*ext);
}

static void vgpu(void *cmd, uint32_t type, uint32_t process)
{
	struct dxgkvmb_command_vgpu_to_host *h = cmd;

	h->command_type = type;
	h->process.v = process;
	h->channel_type = DXGKVMB_VGPU_TO_HOST;
}

static void global(void *cmd, uint32_t type, uint32_t process)
{
	struct dxgkvmb_command_vm_to_host *h = cmd;

	h->command_type = type;
	h->process.v = process;
	h->channel_type = DXGKVMB_VM_TO_HOST;
}

static struct virtio_dxg_resp run(struct dxghost *h, uint32_t cmd_size,
				  uint32_t result_size)
{
	struct virtio_dxg_resp resp;

	dxghost_execute(h, VIRTIO_DXG_CHANNEL_GLOBAL, msg_buf,
			sizeof(struct dxgvmb_ext_header) + cmd_size,
			result_size ? res_buf : NULL, result_size, &resp);
	return resp;
}

static int32_t status_of(void) { return ((struct ntstatus *)res_buf)->v; }

/* --- Tests ------------------------------------------------------------ */

static uint32_t create_process(struct dxghost *h, uint64_t pid)
{
	struct dxgkvmb_command_createprocess *c = msg(sizeof(*c));

	global(c, DXGK_VMBCOMMAND_CREATEPROCESS, 0);
	c->process_id = pid;
	CHECK(run(h, sizeof(*c), 4).status == VIRTIO_DXG_OK);
	return ((struct d3dkmthandle *)res_buf)->v;
}

static uint32_t create_device(struct dxghost *h, uint32_t process)
{
	struct dxgkvmb_command_createdevice *c = msg(sizeof(*c));

	vgpu(c, DXGK_VMBCOMMAND_CREATEDEVICE, process);
	CHECK(run(h, sizeof(*c), 4).status == VIRTIO_DXG_OK);
	return ((struct d3dkmthandle *)res_buf)->v;
}

static void test_executor(void)
{
	struct dxghost_kmt kmt = {0};
	struct dxghost_vmm vmm = {0};
	struct dxghost_config config = {0};
	struct virtio_dxg_config dev;
	struct virtio_dxg_resp resp;
	struct dxghost *h;
	uint32_t p1, p2, d1, d2, alloc1, alloc2, s1, s2;
	uint64_t gpa, fence1, fence2;
	int i;

	kmt.EnumAdapters2 = f_enum;
	kmt.QueryAdapterInfo = f_query;
	kmt.CloseAdapter = f_close_adapter;
	kmt.OpenAdapterFromLuid = f_open_luid;
	kmt.CreateDevice = f_create_device;
	kmt.DestroyDevice = f_destroy_device;
	kmt.ReserveGpuVirtualAddress = f_reserve_va;
	kmt.FreeGpuVirtualAddress = f_free_va;
	kmt.MapGpuVirtualAddress = f_map_va;
	kmt.CreateAllocation2 = f_create_allocation;
	kmt.DestroyAllocation2 = f_destroy_allocation;
	kmt.Lock2 = f_lock2;
	kmt.Unlock2 = f_unlock2;
	kmt.CreateSynchronizationObject2 = f_create_sync;
	kmt.DestroySynchronizationObject = f_destroy_sync;
	kmt.WaitForSynchronizationObjectFromCpu = f_wait_cpu;
	kmt.Escape = f_escape;
	vmm.map = vmm_map;
	vmm.unmap = vmm_unmap;
	vmm.event = vmm_event;
	config.iospace_size = 1ull << 30;
	config.kmt = &kmt;
	fence_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
				  PAGE_READWRITE);

	CHECK(dxghost_create(&config, &vmm, &h) == 0);
	test_h = h;
	dxghost_device_config(h, &dev);
	CHECK(dev.vgpu_luid_low == NV_LUID);	/* NVIDIA, not Intel */
	CHECK(dev.vmbus_version == 40);

	/* Malformed framing never reaches a handler. */
	{
		struct dxgvmb_ext_header *ext = (void *)msg_buf;

		msg(32);
		ext->command_offset = 4000;
		CHECK(run(h, 32, 4).status == VIRTIO_DXG_ERR_INVALID);
		CHECK(run(h, 4, 4).status == VIRTIO_DXG_ERR_INVALID);
	}

	p1 = create_process(h, 100);
	p2 = create_process(h, 200);
	CHECK(p1 && p2 && p1 != p2);

	{
		struct dxgkvmb_command_setiospaceregion *c = msg(sizeof(*c));

		global(c, DXGK_VMBCOMMAND_SETIOSPACEREGION, 0);
		c->start = 0x800000000ull;
		c->length = 1ull << 30;
		run(h, sizeof(*c), 4);
		CHECK(status_of() == 0);
		c = msg(sizeof(*c));
		global(c, DXGK_VMBCOMMAND_SETIOSPACEREGION, 0);
		c->start = 0x900000000ull;
		c->length = 1ull << 30;
		run(h, sizeof(*c), 4);
		CHECK(status_of() == DXG_STATUS_ACCESS_DENIED);
	}
	{
		struct dxgkvmb_command_openadapter *c = msg(sizeof(*c));
		struct dxgkvmb_command_openadapter_return *r = (void *)res_buf;

		vgpu(c, DXGK_VMBCOMMAND_OPENADAPTER, 0);
		c->vmbus_interface_version = 40;
		run(h, sizeof(*c), sizeof(*r));
		CHECK(r->status.v == 0 && r->host_adapter_handle.v == 0x500);
	}
	{
		struct dxgkvmb_command_getinternaladapterinfo *c = msg(sizeof(*c));
		struct dxgkvmb_command_getinternaladapterinfo_return *r =
			(void *)res_buf;

		vgpu(c, DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO, 0);
		resp = run(h, sizeof(*c), sizeof(*r));
		CHECK(resp.status == VIRTIO_DXG_OK);
		CHECK(r->async_msg_enabled && r->hw_sch_enabled);
		CHECK(r->driver_ddi_version == 3200);
		CHECK(r->host_vgpu_luid.a == NV_LUID);
	}
	{
		uint32_t size = 4 + 520;
		struct dxgkvmb_command_queryadapterinfo *c =
			msg(sizeof(*c) - 1 + size);
		const char *want = "libnvwgf2umx.so";
		u16 *path = (u16 *)(res_buf + 4 + 4);
		uint32_t n = 0, base = 0;

		vgpu(c, DXGK_VMBCOMMAND_QUERYADAPTERINFO, p1);
		c->query_type = DXGHOST_QAI_UMDRIVERNAME;
		c->private_data_size = size;
		run(h, sizeof(*c) - 1 + size, 4 + size);
		CHECK(status_of() == 0);
		while (path[n]) {
			if (path[n] == '\\')
				base = n + 1;
			n++;
		}
		for (n = 0; want[n]; n++)
			CHECK(path[base + n] == (u16)want[n]);
		CHECK(path[base + n] == 0);
	}

	d1 = create_device(h, p1);
	d2 = create_device(h, p2);
	CHECK(d1 && d2 && d1 != d2);

	/* Two allocations with private data the KMD rewrites. */
	{
		uint32_t size = sizeof(struct dxgkvmb_command_createallocation) +
				2 * sizeof(struct dxgkvmb_command_createallocation_allocinfo) +
				8;
		struct dxgkvmb_command_createallocation *c = msg(size);
		struct dxgkvmb_command_createallocation_allocinfo *info =
			(void *)&c[1];
		u8 *priv = (u8 *)&info[2];
		struct dxgkvmb_command_createallocation_return *r =
			(void *)res_buf;
		uint32_t res = sizeof(*r) +
			       sizeof(struct dxgkvmb_command_allocinfo_return) + 8;

		vgpu(c, DXGK_VMBCOMMAND_CREATEALLOCATION, p1);
		c->device.v = d1;
		c->alloc_count = 2;
		info[0].priv_drv_data_size = 4;
		info[1].priv_drv_data_size = 4;
		memset(priv, 0x11, 8);
		resp = run(h, size, res);
		CHECK(resp.status == VIRTIO_DXG_OK && resp.size == res);
		alloc1 = r->allocation_info[0].allocation.v;
		alloc2 = r->allocation_info[1].allocation.v;
		CHECK(alloc1 && alloc2 && alloc1 != alloc2);
		priv = (u8 *)&r->allocation_info[2];
		CHECK(priv[0] == (0x11 ^ 0xff) && priv[4] == (0x11 ^ 0xff));
		CHECK(priv[1] == 0x11);
	}

	/* Another process cannot touch it, even through its own device. */
	{
		struct dxgkvmb_command_lock2 *c = msg(sizeof(*c));
		struct dxgkvmb_command_lock2_return_virtio *r = (void *)res_buf;

		vgpu(c, DXGK_VMBCOMMAND_LOCK2, p2);
		c->args.device.v = d2;
		c->args.allocation.v = alloc1;
		run(h, sizeof(*c), sizeof(*r));
		CHECK(r->base.status.v == DXG_STATUS_INVALID_HANDLE);
		CHECK(calls_lock2 == 0);
	}

	/* Lock2 maps once; repeated locks reuse the view. */
	{
		struct dxgkvmb_command_lock2 *c;
		struct dxgkvmb_command_lock2_return_virtio *r = (void *)res_buf;

		for (i = 0; i < 2; i++) {
			c = msg(sizeof(*c));
			vgpu(c, DXGK_VMBCOMMAND_LOCK2, p1);
			c->args.device.v = d1;
			c->args.allocation.v = alloc1;
			run(h, sizeof(*c), sizeof(*r));
			CHECK(r->base.status.v == 0);
			gpa = (uint64_t)(uintptr_t)r->base.cpu_visible_buffer_offset;
			CHECK(gpa >= 0x800000000ull &&
			      gpa < 0x800000000ull + (1ull << 30));
			CHECK(r->mapped_size == 3 * 4096);
			/* A write-combined host view must not be mapped cached. */
			CHECK(!(r->flags & DXGK_VIRTIO_LOCK2_CACHED));
		}
		/* Each lock reaches the driver (it waits for the GPU); the
		 * view is mapped once. */
		CHECK(calls_lock2 == 2 && maps == 1);
		CHECK(last_map_gpa == gpa && last_map_size == 3 * 4096);
	}
	{
		struct dxgkvmb_command_unlock2 *c;
		struct dxgkvmb_command_lock2 *l;
		struct dxgkvmb_command_lock2_return_virtio *r = (void *)res_buf;

		/* First of two unlocks: the driver unlocks, the view stays. */
		c = msg(sizeof(*c));
		vgpu(c, DXGK_VMBCOMMAND_UNLOCK2, p1);
		c->args.device.v = d1;
		c->args.allocation.v = alloc1;
		run(h, sizeof(*c), 4);
		CHECK(status_of() == 0 && calls_unlock2 == 1 && unmaps == 0);
		/* Last unlock: one driver lock and the view stay cached. */
		run(h, sizeof(*c), 4);
		CHECK(status_of() == 0 && calls_unlock2 == 1 && unmaps == 0);
		/* One more unlock than locks is refused. */
		run(h, sizeof(*c), 4);
		CHECK(status_of() == DXG_STATUS_INVALID_PARAMETER);
		/* The next frame's lock and unlock: no map, no unmap. */
		l = msg(sizeof(*l));
		vgpu(l, DXGK_VMBCOMMAND_LOCK2, p1);
		l->args.device.v = d1;
		l->args.allocation.v = alloc1;
		run(h, sizeof(*l), sizeof(*r));
		CHECK(r->base.status.v == 0 && calls_lock2 == 3 && maps == 1 &&
		      (uint64_t)(uintptr_t)r->base.cpu_visible_buffer_offset == gpa);
		c = msg(sizeof(*c));
		vgpu(c, DXGK_VMBCOMMAND_UNLOCK2, p1);
		c->args.device.v = d1;
		c->args.allocation.v = alloc1;
		run(h, sizeof(*c), 4);
		CHECK(status_of() == 0 && calls_unlock2 == 2 && unmaps == 0);
		CHECK(calls_lock2 - calls_unlock2 == 1);	/* the cached lock */
	}

	/* What runs inline on a vCPU thread, and what goes to a lane. */
	{
		struct dxgkvmb_command_lock2 *l;
		struct dxgkvmb_command_unlock2 *u;
		uint32_t locks = calls_lock2;

		/* alloc1's view is cached: Lock2 runs inline. */
		l = msg(sizeof(*l));
		vgpu(l, DXGK_VMBCOMMAND_LOCK2, p1);
		l->args.device.v = d1;
		l->args.allocation.v = alloc1;
		CHECK(dxghost_try_inline(h, VIRTIO_DXG_CHANNEL_GLOBAL, msg_buf,
				 sizeof(struct dxgvmb_ext_header) + sizeof(*l),
				 res_buf, sizeof(struct dxgkvmb_command_lock2_return_virtio),
				 &resp));
		CHECK(resp.status == VIRTIO_DXG_OK && calls_lock2 == locks + 1 &&
		      maps == 1);
		u = msg(sizeof(*u));
		vgpu(u, DXGK_VMBCOMMAND_UNLOCK2, p1);
		u->args.device.v = d1;
		u->args.allocation.v = alloc1;
		CHECK(dxghost_try_inline(h, VIRTIO_DXG_CHANNEL_GLOBAL, msg_buf,
				 sizeof(struct dxgvmb_ext_header) + sizeof(*u),
				 res_buf, 4, &resp));
		CHECK(resp.status == VIRTIO_DXG_OK);
		/* alloc2 has no view: refused, with no driver call made. */
		l = msg(sizeof(*l));
		vgpu(l, DXGK_VMBCOMMAND_LOCK2, p1);
		l->args.device.v = d1;
		l->args.allocation.v = alloc2;
		CHECK(!dxghost_try_inline(h, VIRTIO_DXG_CHANNEL_GLOBAL, msg_buf,
				  sizeof(struct dxgvmb_ext_header) + sizeof(*l),
				  res_buf, sizeof(struct dxgkvmb_command_lock2_return_virtio),
				  &resp));
		CHECK(calls_lock2 == locks + 1);
		/* Creating objects never runs inline. */
		{
			struct dxgkvmb_command_createdevice *c = msg(sizeof(*c));

			vgpu(c, DXGK_VMBCOMMAND_CREATEDEVICE, p1);
			CHECK(!dxghost_try_inline(h, VIRTIO_DXG_CHANNEL_GLOBAL,
					  msg_buf, sizeof(struct dxgvmb_ext_header) +
					  sizeof(*c), res_buf, 4, &resp));
		}
	}

	/* Two fences on one host page share one guest mapping. */
	{
		struct dxgkvmb_command_createsyncobject *c;
		struct dxgkvmb_command_createsyncobject_return *r =
			(void *)res_buf;
		uint32_t before = maps;

		c = msg(sizeof(*c));
		vgpu(c, DXGK_VMBCOMMAND_CREATESYNCOBJECT, p1);
		c->args.device.v = d1;
		c->args.info.type = _D3DDDI_MONITORED_FENCE;
		run(h, sizeof(*c), sizeof(*r));
		s1 = r->sync_object.v;
		fence1 = r->fence_storage_address;
		c = msg(sizeof(*c));
		vgpu(c, DXGK_VMBCOMMAND_CREATESYNCOBJECT, p1);
		c->args.device.v = d1;
		c->args.info.type = _D3DDDI_MONITORED_FENCE;
		run(h, sizeof(*c), sizeof(*r));
		s2 = r->sync_object.v;
		fence2 = r->fence_storage_address;
		CHECK(s1 && s2 && maps == before + 1);
		CHECK((fence1 & ~4095ull) == (fence2 & ~4095ull));
		CHECK((fence1 & 4095) == 0x10 && (fence2 & 4095) == 0x18);
		CHECK(r->fence_gpu_va == 0xabc000);
	}

	/* An async CPU wait signals the guest event from the thread pool. */
	{
		uint32_t size = sizeof(struct dxgkvmb_command_waitforsyncobjectfromcpu) +
				4 + 8;
		struct dxgkvmb_command_waitforsyncobjectfromcpu *c = msg(size);
		u8 *p = (u8 *)&c[1];

		vgpu(c, DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU, p1);
		c->device.v = d1;
		c->object_count = 1;
		c->guest_event_pointer = 0x1234;
		memcpy(p, &s1, 4);
		run(h, size, 4);
		CHECK(status_of() == 0);
		for (i = 0; i < 1000 && !events; i++)
			Sleep(1);
		CHECK(events == 1 && last_event == 0x1234);
	}

	/* An escape failure arrives as an NTSTATUS with the output intact. */
	{
		uint32_t size = sizeof(struct dxgkvmb_command_escape) - 1 + 16;
		struct dxgkvmb_command_escape *c = msg(size);

		vgpu(c, DXGK_VMBCOMMAND_ESCAPE, p1);
		c->adapter.v = 0x500;
		c->device.v = d1;
		c->priv_drv_data_size = 16;
		resp = run(h, size, 16);
		CHECK(resp.status == VIRTIO_DXG_ERR_NTSTATUS);
		CHECK((int32_t)resp.ntstatus == DXG_STATUS_INVALID_PARAMETER);
		CHECK(res_buf[0] == 0x5a);
	}

	/* A handle the guest never received (say, QEMU's own) is refused. */
	{
		struct dxgkvmb_command_destroydevice *c = msg(sizeof(*c));
		uint32_t before = calls_destroy_device;

		vgpu(c, DXGK_VMBCOMMAND_DESTROYDEVICE, p1);
		c->device.v = 0x7777;
		run(h, sizeof(*c), 4);
		CHECK(status_of() == DXG_STATUS_INVALID_HANDLE);
		CHECK(calls_destroy_device == before);
	}

	/* A GPU VA range the process reserves and never frees. */
	{
		struct dxgkvmb_command_reservegpuvirtualaddress *c = msg(sizeof(*c));
		struct dxgkvmb_command_reservegpuvirtualaddress_return *r =
			(void *)res_buf;

		vgpu(c, DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS, p1);
		c->args.adapter.v = 0x500;
		c->args.size = 12ull << 30;
		CHECK(run(h, sizeof(*c), sizeof(*r)).status == VIRTIO_DXG_OK);
		CHECK(r->virtual_address == 0x200000000ull);
	}

	/* A base-less map: the driver frees its range with the allocation,
	 * so process exit must not (another process may own it by then). */
	{
		struct dxgkvmb_command_mapgpuvirtualaddress *c = msg(sizeof(*c));

		AcquireSRWLockExclusive(&h->lock);
		CHECK(dxghost_obj_add(h, 0x7a00, OBJ_PAGING_QUEUE, p1, d1) == 0);
		ReleaseSRWLockExclusive(&h->lock);
		vgpu(c, DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS, p1);
		c->args.paging_queue.v = 0x7a00;
		c->args.allocation.v = alloc1;
		c->args.size_in_pages = 16;
		run(h, sizeof(*c),
		    sizeof(struct dxgkvmb_command_mapgpuvirtualaddress_return));
		CHECK(((struct dxgkvmb_command_mapgpuvirtualaddress_return *)
		       res_buf)->virtual_address == 0x300000000ull);
	}

	/* Process exit destroys its device and releases every mapping, and
	 * the GPU VA it left reserved (the host process owns reservations). */
	{
		struct dxgkvmb_command_destroyprocess *c = msg(sizeof(*c));
		uint32_t before = calls_destroy_device, unmaps_before = unmaps;
		uint32_t frees_before = calls_free_va;
		uint32_t unlocks_before = calls_unlock2;

		global(c, DXGK_VMBCOMMAND_DESTROYPROCESS, p1);
		run(h, sizeof(*c), 4);
		CHECK(status_of() == 0);
		CHECK(calls_destroy_device == before + 1);
		/* The shared fence page and alloc1's cached view, whose driver
		 * lock is released before the device goes. */
		CHECK(unmaps == unmaps_before + 2);
		CHECK(calls_unlock2 == unlocks_before + 1 &&
		      calls_lock2 == calls_unlock2);
		CHECK(calls_free_va == frees_before + 1 &&
		      freed_va_base == 0x200000000ull &&
		      freed_va_size == (12ull << 30));
		AcquireSRWLockExclusive(&h->lock);
		CHECK(!dxghost_obj_find(h, d1) && !dxghost_obj_find(h, alloc2));
		CHECK(dxghost_obj_find(h, d2) != NULL);
		CHECK(h->fence_page_count == 0);
		CHECK(h->io_free_count == 1 &&
		      h->io_free[0].size == (1ull << 30));
		ReleaseSRWLockExclusive(&h->lock);
	}
	/* Its token is dead now. */
	{
		struct dxgkvmb_command_createdevice *c = msg(sizeof(*c));

		vgpu(c, DXGK_VMBCOMMAND_CREATEDEVICE, p1);
		CHECK(run(h, sizeof(*c), 4).status == VIRTIO_DXG_ERR_INVALID);
	}

	/* Teardown stays linear: 20,000 allocations in one process. */
	{
		uint32_t p3 = create_process(h, 300), d3 = create_device(h, p3);
		LARGE_INTEGER start, end, freq;
		uint32_t n;

		for (n = 0; n < 20000; n++) {
			uint32_t size = sizeof(struct dxgkvmb_command_createallocation) +
				sizeof(struct dxgkvmb_command_createallocation_allocinfo);
			struct dxgkvmb_command_createallocation *c = msg(size);

			vgpu(c, DXGK_VMBCOMMAND_CREATEALLOCATION, p3);
			c->device.v = d3;
			c->alloc_count = 1;
			run(h, size, sizeof(struct dxgkvmb_command_createallocation_return));
		}
		CHECK(h->objects.live > 20000);
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		{
			struct dxgkvmb_command_destroyprocess *c = msg(sizeof(*c));

			global(c, DXGK_VMBCOMMAND_DESTROYPROCESS, p3);
			run(h, sizeof(*c), 4);
		}
		QueryPerformanceCounter(&end);
		CHECK((end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart < 500);
		CHECK(!dxghost_obj_find(h, d3));
		printf("teardown of 20000 allocations: %lld ms\n",
		       (long long)((end.QuadPart - start.QuadPart) * 1000 /
				   freq.QuadPart));
	}

	/* Teardown unmaps too: still on this thread, still checked. */
	dxghost_destroy(h);
	test_h = NULL;
	CHECK(lock_violations == 0);
}

static void test_iospace(void)
{
	struct dxghost h;
	uint64_t a, b, c;

	memset(&h, 0, sizeof(h));
	dxghost_io_reset(&h, 0x100000000ull, 64ull << 20);
	a = dxghost_io_alloc(&h, 4096);
	b = dxghost_io_alloc(&h, 4ull << 20);
	c = dxghost_io_alloc(&h, 4096);
	CHECK(a == 0x100000000ull);
	CHECK(b && (b & ((2ull << 20) - 1)) == 0);	/* huge-page aligned */
	CHECK(c && c != a && (c < b || c >= b + (4ull << 20)));
	dxghost_io_free(&h, b, 4ull << 20);
	dxghost_io_free(&h, a, 4096);
	dxghost_io_free(&h, c, 4096);
	CHECK(h.io_free_count == 1 && h.io_free[0].size == (64ull << 20));
	CHECK(dxghost_io_alloc(&h, 128ull << 20) == 0);
	free(h.io_free);
}

int main(void)
{
	test_iospace();
	test_executor();
	if (failures) {
		fprintf(stderr, "executor_test: %d failure(s)\n", failures);
		return 1;
	}
	puts("executor_test: all checks passed");
	return 0;
}
