// SPDX-License-Identifier: MIT
/*
 * D3DKMT on /dev/dxg for Windows code running under Wine.
 *
 * NVIDIA's Windows user-mode drivers call D3DKMT; Linux exposes the same
 * kernel interface as /dev/dxg (dxgkrnl, over Hyper-V or virtio-dxg) with
 * byte-identical argument structures. Each call becomes one ioctl. Strings
 * are UTF-16 on both sides, so no conversion is needed; only Win32 event
 * handles are translated to eventfds.
 *
 * Wine sends system calls made from Windows code to its NT emulation, so
 * the ioctls run in dxgkmt.so, a Wine Unix library loaded by path.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "kmt_bridge.h"
#include "dxgkmt_unix.h"

typedef uint64_t unixlib_handle_t;
static NTSTATUS(WINAPI *unix_dispatch)(unixlib_handle_t, unsigned int, void *);
static unixlib_handle_t unix_lib;

static int64_t unix_call(unsigned int code, void *args, int64_t *ret)
{
	NTSTATUS status = unix_dispatch(unix_lib, code, args);

	return status ? -5 /* EIO */ : *ret;
}

static long linux_ioctl(int fd, unsigned long request, void *arg)
{
	struct dxgkmt_ioctl a = { request, (uint64_t)(uintptr_t)arg, 0 };

	(void)fd;
	return (long)unix_call(DXGKMT_UNIX_IOCTL, &a, &a.ret);
}

static long linux_eventfd(void)
{
	struct dxgkmt_result a = { 0 };

	return (long)unix_call(DXGKMT_UNIX_EVENTFD, &a, &a.ret);
}

/* Waits for an eventfd signal: 0, or -errno. */
static long linux_read_event(int fd)
{
	struct dxgkmt_fd a = { fd, 0 };

	return (long)unix_call(DXGKMT_UNIX_READ_EVENT, &a, &a.ret);
}

static void linux_close(int fd)
{
	struct dxgkmt_fd a = { fd, 0 };

	unix_call(DXGKMT_UNIX_CLOSE, &a, &a.ret);
}

/*
 * Loads dxgkmt.so through Wine; path is a Windows path to the file. This is
 * what winecrt0's __wine_load_unix_lib() does: Wine dlopen()s the library
 * and returns {module, function table} for __wine_unix_call_dispatcher.
 */
#define MEMORY_WINE_LOAD_UNIX_LIB_BY_NAME 1002

static int load_unix_lib(const wchar_t *path)
{
	NTSTATUS(WINAPI *query)(HANDLE, void *, int, void *, SIZE_T, SIZE_T *);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	wchar_t nt_path[MAX_PATH + 8];
	UNICODE_STRING name;
	uint64_t result[2] = { 0, 0 };
	void **dispatcher;
	NTSTATUS status;

	query = (void *)GetProcAddress(ntdll, "NtQueryVirtualMemory");
	dispatcher = (void **)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");
	if (!query || !dispatcher)
		return -38;	/* ENOSYS: not running under Wine */
	unix_dispatch = *dispatcher;
	swprintf(nt_path, MAX_PATH + 8, L"\\??\\%ls", path);
	name.Buffer = nt_path;
	name.Length = (USHORT)(wcslen(nt_path) * sizeof(wchar_t));
	name.MaximumLength = name.Length + sizeof(wchar_t);
	status = query(GetCurrentProcess(), &name,
		       MEMORY_WINE_LOAD_UNIX_LIB_BY_NAME, result, sizeof(result),
		       NULL);
	if (status) {
		fprintf(stderr, "[kmt] loading %ls failed: 0x%08lx\n", path,
			(unsigned long)status);
		return -2;
	}
	unix_lib = result[1];
	return 0;
}

/* Linux _IOWR('G', nr, T) */
#define DXG_IOWR(nr, size) (0xc0000000ul | ((unsigned long)(size) << 16) | \
			    (0x47ul << 8) | (nr))

static int dxg_fd = -1;
static int trace_calls;

struct kmt_stat {
	const char *name;
	volatile LONG calls;
	volatile LONG failures;
};

#define KMT(name, nr, s) static struct kmt_stat stat_##name = { #name };
#include "kmt_table.h"
#undef KMT

static int32_t errno_to_ntstatus(long err)
{
	switch (err) {
	case 17: return (int32_t)0xC0000035;	/* EEXIST: NAME_COLLISION */
	case 12: return (int32_t)0xC0000017;	/* ENOMEM */
	case 22: return (int32_t)0xC000000D;	/* EINVAL */
	case 2: return (int32_t)0xC0000033;	/* ENOENT: NAME_INVALID */
	case 11: return (int32_t)0x00000102;	/* EAGAIN: TIMEOUT */
	case 75: return (int32_t)0xC0000023;	/* EOVERFLOW: BUFFER_TOO_SMALL */
	case 19: return (int32_t)0xC00002B6;	/* ENODEV: DEVICE_REMOVED */
	case 13: return (int32_t)0xC0000022;	/* EACCES */
	case 38: case 1: return (int32_t)0xC00000BB; /* ENOSYS, EPERM */
	case 95: return (int32_t)0xC000001D;	/* EOPNOTSUPP: ILLEGAL_INSTR */
	case 9: return (int32_t)0xC0000008;	/* EBADF: INVALID_HANDLE */
	case 115: return (int32_t)0xC01E0004;	/* EINPROGRESS: ALLOC_BUSY */
	case 91: return (int32_t)0xC0000024;	/* EPROTOTYPE: TYPE_MISMATCH */
	default: return (int32_t)0xC0000001;	/* UNSUCCESSFUL */
	}
}

static int32_t kmt_ioctl(struct kmt_stat *stat, unsigned long request,
			 void *args)
{
	long ret = linux_ioctl(dxg_fd, request, args);
	int32_t status = ret < 0 ? errno_to_ntstatus(-ret) : (int32_t)ret;

	InterlockedIncrement(&stat->calls);
	if (status < 0)
		InterlockedIncrement(&stat->failures);
	if (trace_calls || (status < 0 && trace_calls >= 0))
		fprintf(stderr, "[kmt] D3DKMT%s -> 0x%08x%s\n", stat->name,
			(unsigned)status, ret < 0 ? " (ioctl failed)" : "");
	if (trace_calls > 1) {
		void *frames[10];
		USHORT n = RtlCaptureStackBackTrace(2, 10, frames, NULL), i;

		fprintf(stderr, "[kmt]   from");
		for (i = 0; i < n; i++)
			fprintf(stderr, " %p", frames[i]);
		fprintf(stderr, "\n");
	}
	return status;
}

#define KMT(name, nr, s)						\
	static int32_t WINAPI kmt_##name(void *args)			\
	{								\
		return kmt_ioctl(&stat_##name,				\
				 DXG_IOWR(nr, sizeof(struct s)), args);	\
	}
#include "kmt_table.h"
#undef KMT

/* --- Calls that need more than one ioctl ------------------------------ */

struct async_wait {
	HANDLE event;
	int fd;
};

static DWORD WINAPI async_wait_thread(void *context)
{
	struct async_wait *w = context;

	linux_read_event(w->fd);
	SetEvent(w->event);
	linux_close(w->fd);
	HeapFree(GetProcessHeap(), 0, w);
	return 0;
}

/*
 * A Win32 event becomes an eventfd that dxgkrnl signals; a thread forwards
 * it. (The product version uses one poll thread for all waits.)
 */
static int32_t WINAPI kmt_wait_cpu(void *args)
{
	struct d3dkmt_waitforsynchronizationobjectfromcpu a = *(struct
		d3dkmt_waitforsynchronizationobjectfromcpu *)args;
	HANDLE event = (HANDLE)(uintptr_t)a.async_event;
	struct async_wait *w;
	int32_t status;
	long fd;

	if (!event)
		return kmt_WaitForSynchronizationObjectFromCpu(args);
	fd = linux_eventfd();
	if (fd < 0)
		return (int32_t)0xC0000017;
	a.async_event = (uint64_t)fd;
	status = kmt_WaitForSynchronizationObjectFromCpu(&a);
	if (status < 0) {
		linux_close((int)fd);
		return status;
	}
	w = HeapAlloc(GetProcessHeap(), 0, sizeof(*w));
	w->event = event;
	w->fd = (int)fd;
	CloseHandle(CreateThread(NULL, 64 * 1024, async_wait_thread, w, 0,
				 NULL));
	return status;
}

static int32_t WINAPI kmt_query_adapter_info(void *args)
{
	struct d3dkmt_queryadapterinfo *q = args;
	int32_t status = kmt_QueryAdapterInfo(args);

	if (trace_calls > 0) {
		const uint8_t *p = (const uint8_t *)(uintptr_t)q->private_data;

		fprintf(stderr, "[kmt]   QueryAdapterInfo adapter=%x type=%u "
			"size=%u", q->adapter.v, (unsigned)q->type,
			q->private_data_size);
		if (status >= 0 && q->private_data_size == 4)
			fprintf(stderr, " value=0x%x", *(const uint32_t *)p);
		fprintf(stderr, "\n");
		if (status >= 0 && q->private_data_size > 64) {
			char name[64];
			FILE *f;

			snprintf(name, sizeof(name), "qai-%u-%x.bin",
				 (unsigned)q->type, q->adapter.v);
			f = fopen(name, "wb");
			if (f) {
				fwrite(p, 1, q->private_data_size, f);
				fclose(f);
			}
		}
	}
	if (trace_calls > 0 && q->type == 48 && q->private_data_size >= 548) {
		/* D3DDDI_QUERYREGISTRY_INFO */
		const uint8_t *r = (const uint8_t *)(uintptr_t)q->private_data;
		uint32_t query_type, flags, value_type, out_size, reg_status;

		memcpy(&query_type, r, 4);
		memcpy(&flags, r + 4, 4);
		memcpy(&value_type, r + 528, 4);
		memcpy(&out_size, r + 536, 4);
		memcpy(&reg_status, r + 540, 4);
		fprintf(stderr, "[kmt]   registry query=%u flags=%u value='%ls' "
			"type=%u -> status=%u size=%u", query_type, flags,
			(const wchar_t *)(r + 8), value_type, reg_status,
			out_size);
		if (reg_status == 0 && (value_type == 1 || value_type == 2 ||
					query_type >= 2))
			fprintf(stderr, " '%ls'", (const wchar_t *)(r + 544));
		else if (reg_status == 0 && out_size == 4)
			fprintf(stderr, " dword=%u", *(const uint32_t *)(r + 544));
		fprintf(stderr, "\n");
	}
	return status;
}

static int32_t WINAPI kmt_enum_adapters2(void *args)
{
	struct d3dkmt_enumadapters2 *e = args;
	int32_t status = kmt_EnumAdapters2(args);
	uint32_t i;

	if (trace_calls > 0 && status >= 0 && e->adapters)
		for (i = 0; i < e->num_adapters; i++) {
			const struct d3dkmt_adapterinfo *a =
				(const void *)(uintptr_t)e->adapters + i *
				sizeof(*a);

			fprintf(stderr, "[kmt]   adapter %x luid %08x:%08x "
				"sources %u\n", a->adapter_handle.v,
				a->adapter_luid.b, a->adapter_luid.a,
				a->num_sources);
		}
	return status;
}

static int32_t WINAPI kmt_share_objects(uint32_t count, const uint32_t *objects,
					void *attributes, uint32_t access,
					HANDLE *nt_handle)
{
	struct {
		uint32_t count;
		uint32_t reserved;
		uint64_t objects;
		uint64_t attributes;
		uint32_t access;
		uint32_t reserved2;
		uint64_t nt_handle;
	} a = { count, 0, (uint64_t)(uintptr_t)objects,
		(uint64_t)(uintptr_t)attributes, access, 0,
		(uint64_t)(uintptr_t)nt_handle };

	return kmt_ShareObjectsInternal(&a);
}

/*
 * Present queue limits (D3DKMT_SETQUEUEDLIMIT) bound flips queued through
 * dxgkrnl. Frames leave the VM through the virtual display instead, so
 * there is no dxgkrnl present queue to limit.
 */
static int32_t WINAPI kmt_set_queued_limit(void *args)
{
	(void)args;
	return 0;
}

/*
 * Trim notifications ask the driver to give memory back under pressure.
 * dxgkrnl in a VM reports none, so registration succeeds and the callback
 * is not called; budget changes still arrive through
 * D3DKMTQueryVideoMemoryInfo.
 */
struct register_trim_notification {	/* D3DKMT_REGISTERTRIMNOTIFICATION */
	struct winluid adapter_luid;
	struct d3dkmthandle device;
	void *callback;
	void *context;
	void *handle;
};
_Static_assert(sizeof(struct register_trim_notification) == 40,
	       "D3DKMT_REGISTERTRIMNOTIFICATION");

static int32_t WINAPI kmt_register_trim_notification(void *args)
{
	static volatile LONG next;
	struct register_trim_notification *a = args;

	if (!a->callback)
		return (int32_t)0xC000000D;
	a->handle = (void *)(uintptr_t)(0x7000 + InterlockedIncrement(&next));
	return 0;
}

static int32_t WINAPI kmt_unregister_trim_notification(void *args)
{
	(void)args;
	return 0;
}

/* --- Device idle -------------------------------------------------------- */

/*
 * D3DKMTWaitForIdle has no dxgkrnl ioctl. The bridge remembers each
 * device's contexts and hardware queues; waiting for idle queues a GPU
 * signal of a fresh monitored fence behind the work on each of them, then
 * waits for all those fences from the CPU.
 */
struct tracked {
	struct d3dkmthandle device;
	struct d3dkmthandle object;
	int hwqueue;
};
static struct tracked *tracked;
static uint32_t tracked_count, tracked_capacity;
static SRWLOCK tracked_lock = SRWLOCK_INIT;

static void track(struct d3dkmthandle device, struct d3dkmthandle object,
		  int hwqueue)
{
	AcquireSRWLockExclusive(&tracked_lock);
	if (tracked_count == tracked_capacity) {
		uint32_t capacity = tracked_capacity ? tracked_capacity * 2 : 64;
		void *p = tracked ?
			HeapReAlloc(GetProcessHeap(), 0, tracked,
				    capacity * sizeof(*tracked)) :
			HeapAlloc(GetProcessHeap(), 0,
				  capacity * sizeof(*tracked));

		if (!p)
			goto out;
		tracked = p;
		tracked_capacity = capacity;
	}
	tracked[tracked_count].device = device;
	tracked[tracked_count].object = object;
	tracked[tracked_count].hwqueue = hwqueue;
	tracked_count++;
out:
	ReleaseSRWLockExclusive(&tracked_lock);
}

/* Forgets one object (device.v == 0) or everything of a device. */
static void untrack(struct d3dkmthandle device, struct d3dkmthandle object,
		    int hwqueue)
{
	uint32_t i = 0;

	AcquireSRWLockExclusive(&tracked_lock);
	while (i < tracked_count) {
		struct tracked *t = &tracked[i];

		if (device.v ? t->device.v == device.v :
			       t->object.v == object.v && t->hwqueue == hwqueue)
			*t = tracked[--tracked_count];
		else
			i++;
	}
	ReleaseSRWLockExclusive(&tracked_lock);
}

static struct d3dkmthandle device_of_context(struct d3dkmthandle context)
{
	struct d3dkmthandle device = { { .v = 0 } };
	uint32_t i;

	AcquireSRWLockShared(&tracked_lock);
	for (i = 0; i < tracked_count; i++)
		if (!tracked[i].hwqueue && tracked[i].object.v == context.v)
			device = tracked[i].device;
	ReleaseSRWLockShared(&tracked_lock);
	return device;
}

static int32_t WINAPI kmt_create_context_virtual(void *args)
{
	struct d3dkmt_createcontextvirtual *a = args;
	int32_t status = kmt_CreateContextVirtual(args);

	if (status >= 0)
		track(a->device, a->context, 0);
	return status;
}

static int32_t WINAPI kmt_destroy_context(void *args)
{
	struct d3dkmt_destroycontext *a = args;
	struct d3dkmthandle none = { { .v = 0 } };

	untrack(none, a->context, 0);
	return kmt_DestroyContext(args);
}

static int32_t WINAPI kmt_create_hwqueue(void *args)
{
	struct d3dkmt_createhwqueue *a = args;
	int32_t status = kmt_CreateHwQueue(args);

	if (status >= 0)
		track(device_of_context(a->context), a->queue, 1);
	return status;
}

static int32_t WINAPI kmt_destroy_hwqueue(void *args)
{
	struct d3dkmt_destroyhwqueue *a = args;
	struct d3dkmthandle none = { { .v = 0 } };

	untrack(none, a->queue, 1);
	return kmt_DestroyHwQueue(args);
}

static int32_t WINAPI kmt_destroy_device(void *args)
{
	struct d3dkmt_destroydevice *a = args;

	untrack(a->device, a->device, 0);
	return kmt_DestroyDevice(args);
}

#define D3DDDI_MONITORED_FENCE 5

static int32_t WINAPI kmt_wait_for_idle(void *args)
{
	struct d3dkmthandle device = *(struct d3dkmthandle *)args;
	struct d3dkmthandle *fences;
	struct tracked *work;
	uint64_t *values;
	uint32_t n = 0, armed = 0, i;
	int32_t status = 0;

	AcquireSRWLockShared(&tracked_lock);
	work = HeapAlloc(GetProcessHeap(), 0,
			 (tracked_count + 1) * sizeof(*work));
	for (i = 0; work && i < tracked_count; i++)
		if (tracked[i].device.v == device.v)
			work[n++] = tracked[i];
	ReleaseSRWLockShared(&tracked_lock);
	if (!work)
		return (int32_t)0xC0000017;
	fences = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(*fences));
	values = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(*values));
	for (i = 0; fences && values && i < n; i++) {
		struct d3dkmt_createsynchronizationobject2 c;
		struct d3dkmthandle fence;
		uint64_t one = 1;

		memset(&c, 0, sizeof(c));
		c.device = device;
		c.info.type = D3DDDI_MONITORED_FENCE;
		if (kmt_CreateSynchronizationObject2(&c) < 0)
			continue;
		fence = c.sync_object;
		if (work[i].hwqueue) {
			struct d3dkmt_submitsignalsyncobjectstohwqueue s;

			memset(&s, 0, sizeof(s));
			s.hwqueue_count = 1;
			s.hwqueues = (uint64_t)(uintptr_t)&work[i].object;
			s.object_count = 1;
			s.objects = (uint64_t)(uintptr_t)&fence;
			s.fence_values = (uint64_t)(uintptr_t)&one;
			status = kmt_SubmitSignalSyncObjectsToHwQueue(&s);
		} else {
			struct d3dkmt_signalsynchronizationobjectfromgpu s;

			memset(&s, 0, sizeof(s));
			s.context = work[i].object;
			s.object_count = 1;
			s.objects = (uint64_t)(uintptr_t)&fence;
			s.monitored_fence_values = (uint64_t)(uintptr_t)&one;
			status = kmt_SignalSynchronizationObjectFromGpu(&s);
		}
		if (status < 0) {
			/* A context driven only through its queues refuses
			 * context signals; its queues are waited on instead. */
			struct d3dkmt_destroysynchronizationobject d = { fence };

			kmt_DestroySynchronizationObject(&d);
			continue;
		}
		fences[armed] = fence;
		values[armed++] = 1;
	}
	status = 0;
	if (armed) {
		struct d3dkmt_waitforsynchronizationobjectfromcpu w;

		memset(&w, 0, sizeof(w));
		w.device = device;
		w.object_count = armed;
		w.objects = (uint64_t)(uintptr_t)fences;
		w.fence_values = (uint64_t)(uintptr_t)values;
		status = kmt_WaitForSynchronizationObjectFromCpu(&w);
	}
	for (i = 0; i < armed; i++) {
		struct d3dkmt_destroysynchronizationobject d = { fences[i] };

		kmt_DestroySynchronizationObject(&d);
	}
	HeapFree(GetProcessHeap(), 0, values);
	HeapFree(GetProcessHeap(), 0, fences);
	HeapFree(GetProcessHeap(), 0, work);
	return status;
}

/* Legacy entry points some drivers still resolve. */
static int32_t WINAPI kmt_not_supported(void *args)
{
	(void)args;
	return (int32_t)0xC00000BB;
}

/* --- Lookup ------------------------------------------------------------ */

struct kmt_entry {
	const char *name;
	void *function;
	struct kmt_stat *stat;
};

static int32_t WINAPI kmt_create_sync_object2(void *args);
static int32_t WINAPI kmt_signal_sync_object2(void *args);

static const struct kmt_entry entries[] = {
	{ "WaitForSynchronizationObjectFromCpu", kmt_wait_cpu,
	  &stat_WaitForSynchronizationObjectFromCpu },
	{ "QueryAdapterInfo", kmt_query_adapter_info, &stat_QueryAdapterInfo },
	{ "CreateSynchronizationObject2", kmt_create_sync_object2,
	  &stat_CreateSynchronizationObject2 },
	{ "SignalSynchronizationObject2", kmt_signal_sync_object2,
	  &stat_SignalSynchronizationObject2 },
	{ "CreateContextVirtual", kmt_create_context_virtual,
	  &stat_CreateContextVirtual },
	{ "DestroyContext", kmt_destroy_context, &stat_DestroyContext },
	{ "CreateHwQueue", kmt_create_hwqueue, &stat_CreateHwQueue },
	{ "DestroyHwQueue", kmt_destroy_hwqueue, &stat_DestroyHwQueue },
	{ "DestroyDevice", kmt_destroy_device, &stat_DestroyDevice },
	{ "WaitForIdle", kmt_wait_for_idle, NULL },
	{ "EnumAdapters2", kmt_enum_adapters2, &stat_EnumAdapters2 },
	{ "OpenAdapterFromHdc", kmt_display_open_from_hdc, NULL },
	{ "OpenAdapterFromGdiDisplayName", kmt_display_open_from_name, NULL },
	{ "ShareObjects", kmt_share_objects, &stat_ShareObjectsInternal },
	{ "SetStablePowerState", kmt_not_supported, NULL },
	{ "SetQueuedLimit", kmt_set_queued_limit, NULL },
	{ "RegisterTrimNotification", kmt_register_trim_notification, NULL },
	{ "UnregisterTrimNotification", kmt_unregister_trim_notification,
	  NULL },
#define KMT(name, nr, s) { #name, kmt_##name, &stat_##name },
#include "kmt_table.h"
#undef KMT
};

void *kmt_bridge_lookup(const char *d3dkmt_name)
{
	size_t i;

	if (strncmp(d3dkmt_name, "D3DKMT", 6))
		return NULL;
	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
		if (!strcmp(entries[i].name, d3dkmt_name + 6))
			return entries[i].function;
	return NULL;
}

/*
 * Names the driver resolves that the bridge does not implement get a stub,
 * so lookups never return NULL. A stub logs its name on first use and
 * returns STATUS_NOT_SUPPORTED without touching its arguments.
 */
#define STUB_SLOTS 256
static const char *stub_names[STUB_SLOTS];
static volatile LONG stub_logged[STUB_SLOTS];
static int stub_count;

static int32_t stub_call(int slot, void *caller)
{
	if (trace_calls >= 0 && !InterlockedExchange(&stub_logged[slot], 1))
		fprintf(stderr, "[kmt] D3DKMT%s called from %p: not bridged\n",
			stub_names[slot], caller);
	return (int32_t)0xC00000BB;
}

#define STUB(i) static int32_t WINAPI stub_##i(void *a) { (void)a; \
	return stub_call(i, __builtin_return_address(0)); }
STUB(0)
STUB(1)
STUB(2)
STUB(3)
STUB(4)
STUB(5)
STUB(6)
STUB(7)
STUB(8)
STUB(9)
STUB(10)
STUB(11)
STUB(12)
STUB(13)
STUB(14)
STUB(15)
STUB(16)
STUB(17)
STUB(18)
STUB(19)
STUB(20)
STUB(21)
STUB(22)
STUB(23)
STUB(24)
STUB(25)
STUB(26)
STUB(27)
STUB(28)
STUB(29)
STUB(30)
STUB(31)
STUB(32)
STUB(33)
STUB(34)
STUB(35)
STUB(36)
STUB(37)
STUB(38)
STUB(39)
STUB(40)
STUB(41)
STUB(42)
STUB(43)
STUB(44)
STUB(45)
STUB(46)
STUB(47)
STUB(48)
STUB(49)
STUB(50)
STUB(51)
STUB(52)
STUB(53)
STUB(54)
STUB(55)
STUB(56)
STUB(57)
STUB(58)
STUB(59)
STUB(60)
STUB(61)
STUB(62)
STUB(63)
STUB(64)
STUB(65)
STUB(66)
STUB(67)
STUB(68)
STUB(69)
STUB(70)
STUB(71)
STUB(72)
STUB(73)
STUB(74)
STUB(75)
STUB(76)
STUB(77)
STUB(78)
STUB(79)
STUB(80)
STUB(81)
STUB(82)
STUB(83)
STUB(84)
STUB(85)
STUB(86)
STUB(87)
STUB(88)
STUB(89)
STUB(90)
STUB(91)
STUB(92)
STUB(93)
STUB(94)
STUB(95)
STUB(96)
STUB(97)
STUB(98)
STUB(99)
STUB(100)
STUB(101)
STUB(102)
STUB(103)
STUB(104)
STUB(105)
STUB(106)
STUB(107)
STUB(108)
STUB(109)
STUB(110)
STUB(111)
STUB(112)
STUB(113)
STUB(114)
STUB(115)
STUB(116)
STUB(117)
STUB(118)
STUB(119)
STUB(120)
STUB(121)
STUB(122)
STUB(123)
STUB(124)
STUB(125)
STUB(126)
STUB(127)
STUB(128)
STUB(129)
STUB(130)
STUB(131)
STUB(132)
STUB(133)
STUB(134)
STUB(135)
STUB(136)
STUB(137)
STUB(138)
STUB(139)
STUB(140)
STUB(141)
STUB(142)
STUB(143)
STUB(144)
STUB(145)
STUB(146)
STUB(147)
STUB(148)
STUB(149)
STUB(150)
STUB(151)
STUB(152)
STUB(153)
STUB(154)
STUB(155)
STUB(156)
STUB(157)
STUB(158)
STUB(159)
STUB(160)
STUB(161)
STUB(162)
STUB(163)
STUB(164)
STUB(165)
STUB(166)
STUB(167)
STUB(168)
STUB(169)
STUB(170)
STUB(171)
STUB(172)
STUB(173)
STUB(174)
STUB(175)
STUB(176)
STUB(177)
STUB(178)
STUB(179)
STUB(180)
STUB(181)
STUB(182)
STUB(183)
STUB(184)
STUB(185)
STUB(186)
STUB(187)
STUB(188)
STUB(189)
STUB(190)
STUB(191)
STUB(192)
STUB(193)
STUB(194)
STUB(195)
STUB(196)
STUB(197)
STUB(198)
STUB(199)
STUB(200)
STUB(201)
STUB(202)
STUB(203)
STUB(204)
STUB(205)
STUB(206)
STUB(207)
STUB(208)
STUB(209)
STUB(210)
STUB(211)
STUB(212)
STUB(213)
STUB(214)
STUB(215)
STUB(216)
STUB(217)
STUB(218)
STUB(219)
STUB(220)
STUB(221)
STUB(222)
STUB(223)
STUB(224)
STUB(225)
STUB(226)
STUB(227)
STUB(228)
STUB(229)
STUB(230)
STUB(231)
STUB(232)
STUB(233)
STUB(234)
STUB(235)
STUB(236)
STUB(237)
STUB(238)
STUB(239)
STUB(240)
STUB(241)
STUB(242)
STUB(243)
STUB(244)
STUB(245)
STUB(246)
STUB(247)
STUB(248)
STUB(249)
STUB(250)
STUB(251)
STUB(252)
STUB(253)
STUB(254)
STUB(255)
static void *const stub_functions[STUB_SLOTS] = {
	stub_0,
	stub_1,
	stub_2,
	stub_3,
	stub_4,
	stub_5,
	stub_6,
	stub_7,
	stub_8,
	stub_9,
	stub_10,
	stub_11,
	stub_12,
	stub_13,
	stub_14,
	stub_15,
	stub_16,
	stub_17,
	stub_18,
	stub_19,
	stub_20,
	stub_21,
	stub_22,
	stub_23,
	stub_24,
	stub_25,
	stub_26,
	stub_27,
	stub_28,
	stub_29,
	stub_30,
	stub_31,
	stub_32,
	stub_33,
	stub_34,
	stub_35,
	stub_36,
	stub_37,
	stub_38,
	stub_39,
	stub_40,
	stub_41,
	stub_42,
	stub_43,
	stub_44,
	stub_45,
	stub_46,
	stub_47,
	stub_48,
	stub_49,
	stub_50,
	stub_51,
	stub_52,
	stub_53,
	stub_54,
	stub_55,
	stub_56,
	stub_57,
	stub_58,
	stub_59,
	stub_60,
	stub_61,
	stub_62,
	stub_63,
	stub_64,
	stub_65,
	stub_66,
	stub_67,
	stub_68,
	stub_69,
	stub_70,
	stub_71,
	stub_72,
	stub_73,
	stub_74,
	stub_75,
	stub_76,
	stub_77,
	stub_78,
	stub_79,
	stub_80,
	stub_81,
	stub_82,
	stub_83,
	stub_84,
	stub_85,
	stub_86,
	stub_87,
	stub_88,
	stub_89,
	stub_90,
	stub_91,
	stub_92,
	stub_93,
	stub_94,
	stub_95,
	stub_96,
	stub_97,
	stub_98,
	stub_99,
	stub_100,
	stub_101,
	stub_102,
	stub_103,
	stub_104,
	stub_105,
	stub_106,
	stub_107,
	stub_108,
	stub_109,
	stub_110,
	stub_111,
	stub_112,
	stub_113,
	stub_114,
	stub_115,
	stub_116,
	stub_117,
	stub_118,
	stub_119,
	stub_120,
	stub_121,
	stub_122,
	stub_123,
	stub_124,
	stub_125,
	stub_126,
	stub_127,
	stub_128,
	stub_129,
	stub_130,
	stub_131,
	stub_132,
	stub_133,
	stub_134,
	stub_135,
	stub_136,
	stub_137,
	stub_138,
	stub_139,
	stub_140,
	stub_141,
	stub_142,
	stub_143,
	stub_144,
	stub_145,
	stub_146,
	stub_147,
	stub_148,
	stub_149,
	stub_150,
	stub_151,
	stub_152,
	stub_153,
	stub_154,
	stub_155,
	stub_156,
	stub_157,
	stub_158,
	stub_159,
	stub_160,
	stub_161,
	stub_162,
	stub_163,
	stub_164,
	stub_165,
	stub_166,
	stub_167,
	stub_168,
	stub_169,
	stub_170,
	stub_171,
	stub_172,
	stub_173,
	stub_174,
	stub_175,
	stub_176,
	stub_177,
	stub_178,
	stub_179,
	stub_180,
	stub_181,
	stub_182,
	stub_183,
	stub_184,
	stub_185,
	stub_186,
	stub_187,
	stub_188,
	stub_189,
	stub_190,
	stub_191,
	stub_192,
	stub_193,
	stub_194,
	stub_195,
	stub_196,
	stub_197,
	stub_198,
	stub_199,
	stub_200,
	stub_201,
	stub_202,
	stub_203,
	stub_204,
	stub_205,
	stub_206,
	stub_207,
	stub_208,
	stub_209,
	stub_210,
	stub_211,
	stub_212,
	stub_213,
	stub_214,
	stub_215,
	stub_216,
	stub_217,
	stub_218,
	stub_219,
	stub_220,
	stub_221,
	stub_222,
	stub_223,
	stub_224,
	stub_225,
	stub_226,
	stub_227,
	stub_228,
	stub_229,
	stub_230,
	stub_231,
	stub_232,
	stub_233,
	stub_234,
	stub_235,
	stub_236,
	stub_237,
	stub_238,
	stub_239,
	stub_240,
	stub_241,
	stub_242,
	stub_243,
	stub_244,
	stub_245,
	stub_246,
	stub_247,
	stub_248,
	stub_249,
	stub_250,
	stub_251,
	stub_252,
	stub_253,
	stub_254,
	stub_255,
};

/* Legacy v1 calls with direct v2 equivalents, as libdxg maps them. */
static int32_t WINAPI kmt_destroy_allocation_v1(void *args)
{
	struct d3dkmt_destroyallocation2 a = { 0 };

	memcpy(&a, args, offsetof(struct d3dkmt_destroyallocation2, flags));
	return kmt_DestroyAllocation2(&a);
}

/*
 * D3DKMTCreateAllocation takes the same arguments as CreateAllocation2 but
 * D3DDDI_ALLOCATIONINFO entries, which lack the GPU VA, priority and
 * reserved fields of D3DDDI_ALLOCATIONINFO2. The entries are widened for
 * the call and the allocation handles copied back.
 */
struct allocationinfo_v1 {	/* D3DDDI_ALLOCATIONINFO */
	struct d3dkmthandle allocation;
	uint64_t sysmem;
	uint64_t priv_drv_data;
	uint32_t priv_drv_data_size;
	uint32_t vidpn_source_id;
	uint32_t flags;
};
_Static_assert(sizeof(struct allocationinfo_v1) == 40, "D3DDDI_ALLOCATIONINFO");
_Static_assert(sizeof(struct d3dddi_allocationinfo2) == 96,
	       "D3DDDI_ALLOCATIONINFO2");

/*
 * A CPU notification object signals its Win32 event each time the GPU
 * reaches it; dxgkrnl signals an eventfd instead. A thread per object
 * forwards the signals. It outlives the object: the prototype has no way
 * to wake it (the product version uses one epoll thread for all objects
 * and removes an object's eventfd when it is destroyed).
 */
struct cpu_notification {
	HANDLE event;
	int fd;
};

static DWORD WINAPI cpu_notification_thread(void *context)
{
	struct cpu_notification *n = context;

	while (linux_read_event(n->fd) == 0)
		SetEvent(n->event);
	linux_close(n->fd);
	HeapFree(GetProcessHeap(), 0, n);
	return 0;
}

#define D3DDDI_CPU_NOTIFICATION 4

static int32_t WINAPI kmt_create_sync_object2(void *args)
{
	struct d3dkmt_createsynchronizationobject2 *a = args;
	struct cpu_notification *n;
	uint64_t event = a->info.cpu_notification.event;
	int32_t status;
	long fd;

	if (trace_calls > 0)
		fprintf(stderr, "[kmt]   sync object type %u flags 0x%x\n",
			(unsigned)a->info.type, *(uint32_t *)&a->info.flags);
	if (a->info.type != D3DDDI_CPU_NOTIFICATION)
		return kmt_CreateSynchronizationObject2(args);
	fd = linux_eventfd();
	if (fd < 0)
		return (int32_t)0xC0000017;
	a->info.cpu_notification.event = (uint64_t)fd;
	status = kmt_CreateSynchronizationObject2(args);
	a->info.cpu_notification.event = event;
	if (status < 0) {
		linux_close((int)fd);
		return status;
	}
	n = HeapAlloc(GetProcessHeap(), 0, sizeof(*n));
	n->event = (HANDLE)(uintptr_t)event;
	n->fd = (int)fd;
	CloseHandle(CreateThread(NULL, 64 * 1024, cpu_notification_thread, n,
				 0, NULL));
	return status;
}

/*
 * SignalSynchronizationObject2 with EnqueueCpuEvent signals a Win32 event
 * once the GPU processes the signal; dxgkrnl takes an eventfd.
 */
static int32_t WINAPI kmt_signal_sync_object2(void *args)
{
	struct d3dkmt_signalsynchronizationobject2 *a = args;
	struct async_wait *w;
	uint64_t event = a->cpu_event_handle;
	int32_t status;
	long fd;

	if (!a->flags.enqueue_cpu_event)
		return kmt_SignalSynchronizationObject2(args);
	fd = linux_eventfd();
	if (fd < 0)
		return (int32_t)0xC0000017;
	a->cpu_event_handle = (uint64_t)fd;
	status = kmt_SignalSynchronizationObject2(args);
	a->cpu_event_handle = event;
	if (status < 0) {
		linux_close((int)fd);
		return status;
	}
	w = HeapAlloc(GetProcessHeap(), 0, sizeof(*w));
	w->event = (HANDLE)(uintptr_t)event;
	w->fd = (int)fd;
	CloseHandle(CreateThread(NULL, 64 * 1024, async_wait_thread, w, 0,
				 NULL));
	return status;
}

/*
 * The v1 signal and wait structures are prefixes of the v2 ones; v2 adds
 * broadcast contexts and a fence value, unused by v1 callers.
 */
struct signal_sync_object_v1 {	/* D3DKMT_SIGNALSYNCHRONIZATIONOBJECT */
	struct d3dkmthandle context;
	uint32_t object_count;
	struct d3dkmthandle objects[D3DDDI_MAX_OBJECT_SIGNALED];
	struct d3dddicb_signalflags flags;
};

static int32_t WINAPI kmt_signal_sync_object_v1(void *args)
{
	struct d3dkmt_signalsynchronizationobject2 v2;

	memset(&v2, 0, sizeof(v2));
	memcpy(&v2, args, sizeof(struct signal_sync_object_v1));
	return kmt_signal_sync_object2(&v2);
}

struct wait_sync_object_v1 {	/* D3DKMT_WAITFORSYNCHRONIZATIONOBJECT */
	struct d3dkmthandle context;
	uint32_t object_count;
	struct d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
};

static int32_t WINAPI kmt_wait_sync_object_v1(void *args)
{
	struct d3dkmt_waitforsynchronizationobject2 v2;

	memset(&v2, 0, sizeof(v2));
	memcpy(&v2, args, sizeof(struct wait_sync_object_v1));
	return kmt_WaitForSynchronizationObject2(&v2);
}

/*
 * The process scheduling class only orders this process against others on
 * the host GPU; the VM's processes share one host process, whose class the
 * host sets. Reads report NORMAL, writes are accepted and remembered.
 */
static volatile LONG process_priority_class = 2;	/* NORMAL */

static int32_t WINAPI kmt_get_process_priority_class(HANDLE process,
						     LONG *priority_class)
{
	(void)process;
	if (!priority_class)
		return (int32_t)0xC000000D;
	*priority_class = process_priority_class;
	return 0;
}

static int32_t WINAPI kmt_set_process_priority_class(HANDLE process,
						     LONG priority_class)
{
	(void)process;
	if (priority_class < 0 || priority_class > 5)
		return (int32_t)0xC000000D;
	InterlockedExchange(&process_priority_class, priority_class);
	return 0;
}

/* D3DKMTCreateSynchronizationObject: the v1 info has no flags field. */
struct create_sync_object_v1 {	/* D3DKMT_CREATESYNCHRONIZATIONOBJECT */
	struct d3dkmthandle device;
	uint32_t type;
	uint32_t info[16];
	struct d3dkmthandle sync_object;
};
_Static_assert(sizeof(struct create_sync_object_v1) == 76,
	       "D3DKMT_CREATESYNCHRONIZATIONOBJECT");

static int32_t WINAPI kmt_create_sync_object_v1(void *args)
{
	struct create_sync_object_v1 *v1 = args;
	struct d3dkmt_createsynchronizationobject2 v2;
	int32_t status;

	memset(&v2, 0, sizeof(v2));
	v2.device = v1->device;
	v2.info.type = v1->type;
	memcpy(&v2.info.reserved, v1->info, sizeof(v1->info));
	status = kmt_create_sync_object2(&v2);
	v1->sync_object = v2.sync_object;
	return status;
}

static int32_t WINAPI kmt_create_allocation_v1(void *args)
{
	struct d3dkmt_createallocation *a = args;
	struct allocationinfo_v1 *v1 = (void *)(uintptr_t)a->allocation_info;
	struct d3dddi_allocationinfo2 small[4], *v2 = small;
	int32_t status;
	uint32_t i;

	if (a->alloc_count > 4) {
		v2 = HeapAlloc(GetProcessHeap(), 0, a->alloc_count * sizeof(*v2));
		if (!v2)
			return (int32_t)0xC0000017;
	}
	memset(v2, 0, a->alloc_count * sizeof(*v2));
	for (i = 0; i < a->alloc_count; i++) {
		v2[i].allocation = v1[i].allocation;
		v2[i].sysmem = v1[i].sysmem;
		v2[i].priv_drv_data = v1[i].priv_drv_data;
		v2[i].priv_drv_data_size = v1[i].priv_drv_data_size;
		v2[i].vidpn_source_id = v1[i].vidpn_source_id;
		v2[i].flags.value = v1[i].flags;
	}
	a->allocation_info = (uint64_t)(uintptr_t)v2;
	status = kmt_CreateAllocation2(a);
	a->allocation_info = (uint64_t)(uintptr_t)v1;
	for (i = 0; i < a->alloc_count; i++)
		v1[i].allocation = v2[i].allocation;
	if (v2 != small)
		HeapFree(GetProcessHeap(), 0, v2);
	return status;
}

void *kmt_bridge_resolve(const char *d3dkmt_name)
{
	void *f = kmt_bridge_lookup(d3dkmt_name);
	int i;

	if (f || strncmp(d3dkmt_name, "D3DKMT", 6))
		return f;
	if (!strcmp(d3dkmt_name, "D3DKMTDestroyAllocation"))
		return kmt_destroy_allocation_v1;
	if (!strcmp(d3dkmt_name, "D3DKMTCreateAllocation"))
		return kmt_create_allocation_v1;
	if (!strcmp(d3dkmt_name, "D3DKMTCreateSynchronizationObject"))
		return kmt_create_sync_object_v1;
	if (!strcmp(d3dkmt_name, "D3DKMTSignalSynchronizationObject"))
		return kmt_signal_sync_object_v1;
	if (!strcmp(d3dkmt_name, "D3DKMTWaitForSynchronizationObject"))
		return kmt_wait_sync_object_v1;
	if (!strcmp(d3dkmt_name, "D3DKMTGetProcessSchedulingPriorityClass"))
		return kmt_get_process_priority_class;
	if (!strcmp(d3dkmt_name, "D3DKMTSetProcessSchedulingPriorityClass"))
		return kmt_set_process_priority_class;
	for (i = 0; i < stub_count; i++)
		if (!strcmp(stub_names[i], d3dkmt_name + 6))
			return stub_functions[i];
	if (stub_count == STUB_SLOTS)
		return NULL;
	stub_names[stub_count] = _strdup(d3dkmt_name + 6);
	return stub_functions[stub_count++];
}

int kmt_bridge_open(const wchar_t *unix_lib_path, int trace)
{
	struct dxgkmt_result a = { 0 };
	int ret;

	trace_calls = trace;
	ret = load_unix_lib(unix_lib_path);
	if (ret)
		return ret;
	ret = (int)unix_call(DXGKMT_UNIX_OPEN, &a, &a.ret);
	dxg_fd = ret < 0 ? -1 : 0;
	return ret;
}

void kmt_bridge_report(void)
{
	size_t i, j;

	fprintf(stderr, "[kmt] calls by function:\n");
	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct kmt_stat *s = entries[i].stat;

		/* Wrappers share the counters of the ioctl they call. */
		for (j = 0; j < i && entries[j].stat != s; j++)
			;
		if (s && s->calls && j == i)
			fprintf(stderr, "[kmt]   %-40s %6ld  failed %ld\n",
				s->name, s->calls, s->failures);
	}
}
