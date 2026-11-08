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

static void linux_read_event(int fd)
{
	struct dxgkmt_fd a = { fd, 0 };

	unix_call(DXGKMT_UNIX_READ_EVENT, &a, &a.ret);
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

	if (trace_calls > 0)
		fprintf(stderr, "[kmt]   QueryAdapterInfo type=%u size=%u\n",
			(unsigned)q->type, q->private_data_size);
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

static const struct kmt_entry entries[] = {
	{ "WaitForSynchronizationObjectFromCpu", kmt_wait_cpu,
	  &stat_WaitForSynchronizationObjectFromCpu },
	{ "QueryAdapterInfo", kmt_query_adapter_info, &stat_QueryAdapterInfo },
	{ "ShareObjects", kmt_share_objects, &stat_ShareObjectsInternal },
	{ "SetStablePowerState", kmt_not_supported, NULL },
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
	size_t i;

	fprintf(stderr, "[kmt] calls by function:\n");
	for (i = 3; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct kmt_stat *s = entries[i].stat;

		if (s && s->calls)
			fprintf(stderr, "[kmt]   %-40s %6ld  failed %ld\n",
				s->name, s->calls, s->failures);
	}
}
