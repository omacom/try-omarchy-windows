// SPDX-License-Identifier: MIT
/*
 * nvvkprobe: runs NVIDIA's Windows Vulkan driver inside Linux.
 *
 * Under Wine, loads the vendor ICD from the host driver store (mounted in
 * the guest), routes its D3DKMT calls to /dev/dxg through kmt_bridge.c, and
 * exercises it: instance, physical device, feature report, then a compute
 * dispatch whose every output is checked on the CPU.
 *
 *   wine nvvkprobe.exe [--trace] [path\to\nvoglv64.dll]
 */
#include <windows.h>
#include <winternl.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "kmt_bridge.h"
#include "probe_compute_spv.h"

static int trace;
/* --native: the same test on Windows itself, as the baseline. */
static int native;

/* --- Routing the driver's kernel calls --------------------------------- */

static FARPROC(WINAPI *real_GetProcAddress)(HMODULE, LPCSTR);
static HMODULE(WINAPI *real_LoadLibraryA)(LPCSTR);
static HMODULE(WINAPI *real_LoadLibraryW)(LPCWSTR);
static HMODULE(WINAPI *real_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
static HMODULE(WINAPI *real_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static wchar_t driver_dir[MAX_PATH];
static HMODULE patched[64];
static int patched_count;

static void patch_module(HMODULE module);

static void *bridged(const char *name)
{
	char d3dkmt[128];

	if (!strncmp(name, "NtGdiDdDDI", 10)) {
		snprintf(d3dkmt, sizeof(d3dkmt), "D3DKMT%s", name + 10);
		name = d3dkmt;
	}
	return kmt_bridge_resolve(name);
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name)
{
	void *display = (uintptr_t)name > 0xffff ? kmt_display_resolve(name)
						 : NULL;

	if (display)
		return (FARPROC)display;
	if ((uintptr_t)name > 0xffff &&
	    (!strncmp(name, "D3DKMT", 6) || !strncmp(name, "NtGdiDdDDI", 10))) {
		void *f = bridged(name);

		if (trace || !f)
			fprintf(stderr, "[probe] driver resolves %s -> %s\n",
				name, f ? "bridge" : "Wine");
		if (f)
			return (FARPROC)f;
	}
	return real_GetProcAddress(module, name);
}

static void patch_if_driver(HMODULE module)
{
	wchar_t path[MAX_PATH];

	if (module && GetModuleFileNameW(module, path, MAX_PATH) &&
	    !_wcsnicmp(path, driver_dir, wcslen(driver_dir)))
		patch_module(module);
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name)
{
	HMODULE m = real_LoadLibraryA(name);

	patch_if_driver(m);
	return m;
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
	HMODULE m = real_LoadLibraryW(name);

	patch_if_driver(m);
	return m;
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
	HMODULE m = real_LoadLibraryExA(name, file, flags);

	patch_if_driver(m);
	return m;
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
	HMODULE m = real_LoadLibraryExW(name, file, flags);

	patch_if_driver(m);
	return m;
}

static void *monitored(const char *name);

static void *replacement(const char *dll, const char *name)
{
	(void)dll;
	if (trace) {
		void *w = monitored(name);

		if (w)
			return w;
	}
	if (!strcmp(name, "GetProcAddress"))
		return hook_GetProcAddress;
	if (!strcmp(name, "LoadLibraryA"))
		return hook_LoadLibraryA;
	if (!strcmp(name, "LoadLibraryW"))
		return hook_LoadLibraryW;
	if (!strcmp(name, "LoadLibraryExA"))
		return hook_LoadLibraryExA;
	if (!strcmp(name, "LoadLibraryExW"))
		return hook_LoadLibraryExW;
	if (!strncmp(name, "D3DKMT", 6) || !strncmp(name, "NtGdiDdDDI", 10))
		return bridged(name);
	return kmt_display_resolve(name);
}

/* Rewrites the module's import address table entries we route. */
static void patch_module(HMODULE module)
{
	BYTE *base = (BYTE *)module;
	IMAGE_NT_HEADERS64 *nt;
	IMAGE_DATA_DIRECTORY dir;
	IMAGE_IMPORT_DESCRIPTOR *desc;
	int i, routed = 0;

	for (i = 0; i < patched_count; i++)
		if (patched[i] == module)
			return;
	if (patched_count < 64)
		patched[patched_count++] = module;
	nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return;
	for (desc = (void *)(base + dir.VirtualAddress); desc->Name; desc++) {
		const char *dll = (const char *)(base + desc->Name);
		IMAGE_THUNK_DATA64 *names = (void *)(base +
			(desc->OriginalFirstThunk ? desc->OriginalFirstThunk :
						    desc->FirstThunk));
		IMAGE_THUNK_DATA64 *iat = (void *)(base + desc->FirstThunk);

		for (; names->u1.AddressOfData; names++, iat++) {
			IMAGE_IMPORT_BY_NAME *ibn;
			void *with;
			DWORD old;

			if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
				continue;
			ibn = (void *)(base + names->u1.AddressOfData);
			with = replacement(dll, (const char *)ibn->Name);
			if (!with)
				continue;
			VirtualProtect(&iat->u1.Function, sizeof(void *),
				       PAGE_READWRITE, &old);
			iat->u1.Function = (ULONGLONG)(uintptr_t)with;
			VirtualProtect(&iat->u1.Function, sizeof(void *), old,
				       &old);
			routed++;
		}
	}
	if (trace) {
		wchar_t path[MAX_PATH] = L"?";

		GetModuleFileNameW(module, path, MAX_PATH);
		fprintf(stderr, "[probe] routed %d imports of %ls\n", routed,
			path);
	}
}

/*
 * The driver resolves its D3DKMT table in DllMain, before its imports can
 * be patched, so GetProcAddress is replaced process-wide before loading it.
 * The replacement is complete (it calls LdrGetProcedureAddress itself), so
 * no trampoline to the original is needed.
 */
static NTSTATUS(WINAPI *ldr_get_procedure)(HMODULE, const ANSI_STRING *, ULONG,
					   void **);
static ULONG(WINAPI *nt_status_to_dos)(NTSTATUS);

/*
 * API monitor for --trace: functions the driver resolves at run time
 * bypass Wine's relay log, so the interesting ones are wrapped here.
 */
static int from_driver(void *address)
{
	wchar_t path[MAX_PATH];
	HMODULE m;

	if (!*driver_dir || !GetModuleHandleExW(6, address, &m) ||
	    !GetModuleFileNameW(m, path, MAX_PATH))
		return 0;
	return !_wcsnicmp(path, driver_dir, wcslen(driver_dir));
}

#define API_LOG(fmt, ...)						\
	do {								\
		DWORD e_ = GetLastError();				\
		fprintf(stderr, "[api] " fmt " err=%lu\n", __VA_ARGS__, e_); \
		SetLastError(e_);					\
	} while (0)

static HANDLE WINAPI log_CreateFileW(LPCWSTR n, DWORD acc, DWORD share,
				     LPSECURITY_ATTRIBUTES sa, DWORD disp,
				     DWORD flags, HANDLE t)
{
	HANDLE h = CreateFileW(n, acc, share, sa, disp, flags, t);

	API_LOG("CreateFileW(%ls, access=%lx, share=%lx, disp=%lu) -> %p", n,
		acc, share, disp, h);
	return h;
}

static DWORD WINAPI log_GetFileAttributesW(LPCWSTR n)
{
	DWORD a = GetFileAttributesW(n);

	API_LOG("GetFileAttributesW(%ls) -> %lx", n, a);
	return a;
}

static HANDLE WINAPI log_CreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa,
					    DWORD prot, DWORD hi, DWORD lo,
					    LPCWSTR name)
{
	HANDLE h = CreateFileMappingW(f, sa, prot, hi, lo, name);

	API_LOG("CreateFileMappingW(%p, prot=%lx, size=%lu, %ls) -> %p", f,
		prot, lo, name ? name : L"", h);
	return h;
}

static HANDLE WINAPI log_OpenFileMappingW(DWORD acc, BOOL inherit, LPCWSTR name)
{
	HANDLE h = OpenFileMappingW(acc, inherit, name);

	API_LOG("OpenFileMappingW(%lx, %ls) -> %p", acc, name, h);
	return h;
}

static HANDLE WINAPI log_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner,
				      LPCWSTR name)
{
	HANDLE h = CreateMutexW(sa, owner, name);

	API_LOG("CreateMutexW(%ls) -> %p", name ? name : L"", h);
	return h;
}

static HANDLE WINAPI log_OpenMutexW(DWORD acc, BOOL inherit, LPCWSTR name)
{
	HANDLE h = OpenMutexW(acc, inherit, name);

	API_LOG("OpenMutexW(%ls) -> %p", name, h);
	return h;
}

static HANDLE WINAPI log_CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual,
				      BOOL initial, LPCWSTR name)
{
	HANDLE h = CreateEventW(sa, manual, initial, name);

	API_LOG("CreateEventW(%ls) -> %p", name ? name : L"", h);
	return h;
}

static LSTATUS WINAPI log_RegOpenKeyExW(HKEY k, LPCWSTR sub, DWORD opt,
					REGSAM sam, PHKEY out)
{
	LSTATUS s = RegOpenKeyExW(k, sub, opt, sam, out);

	API_LOG("RegOpenKeyExW(%p, %ls, sam=%lx) -> %ld", (void *)k,
		sub ? sub : L"", sam, s);
	return s;
}

static LSTATUS WINAPI log_RegQueryValueExW(HKEY k, LPCWSTR name, LPDWORD r,
					   LPDWORD type, LPBYTE data,
					   LPDWORD size)
{
	LSTATUS s = RegQueryValueExW(k, name, r, type, data, size);

	API_LOG("RegQueryValueExW(%p, %ls) -> %ld", (void *)k,
		name ? name : L"", s);
	return s;
}

static BOOL WINAPI log_DeviceIoControl(HANDLE h, DWORD code, LPVOID in,
				       DWORD in_size, LPVOID out,
				       DWORD out_size, LPDWORD ret,
				       LPOVERLAPPED ov)
{
	BOOL ok = DeviceIoControl(h, code, in, in_size, out, out_size, ret, ov);

	API_LOG("DeviceIoControl(%p, code=%lx) -> %d", h, code, ok);
	return ok;
}

static UINT WINAPI log_GetSystemDirectoryW(LPWSTR buf, UINT size)
{
	UINT n = GetSystemDirectoryW(buf, size);

	API_LOG("GetSystemDirectoryW -> %u %ls", n, n && buf ? buf : L"");
	return n;
}

static HMODULE WINAPI log_GetModuleHandleW(LPCWSTR name)
{
	HMODULE m = GetModuleHandleW(name);

	API_LOG("GetModuleHandleW(%ls) -> %p", name ? name : L"(exe)",
		(void *)m);
	return m;
}

static DWORD WINAPI log_GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD size)
{
	DWORD n = GetModuleFileNameW(m, buf, size);

	API_LOG("GetModuleFileNameW(%p) -> %lu %ls", (void *)m, n,
		n ? buf : L"");
	return n;
}

static DWORD WINAPI log_GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf,
						DWORD size)
{
	DWORD n = GetEnvironmentVariableW(name, buf, size);

	API_LOG("GetEnvironmentVariableW(%ls) -> %lu", name, n);
	return n;
}

static HANDLE WINAPI log_OpenEventW(DWORD acc, BOOL inherit, LPCWSTR name)
{
	HANDLE h = OpenEventW(acc, inherit, name);

	API_LOG("OpenEventW(%ls) -> %p", name, h);
	return h;
}

static BOOL WINAPI log_ReadFile(HANDLE h, LPVOID buf, DWORD size, LPDWORD done,
				LPOVERLAPPED ov)
{
	BOOL ok = ReadFile(h, buf, size, done, ov);

	API_LOG("ReadFile(%p, %lu) -> %d read=%lu", h, size, ok,
		done ? *done : 0);
	return ok;
}

static BOOL WINAPI log_WriteFile(HANDLE h, LPCVOID buf, DWORD size,
				 LPDWORD done, LPOVERLAPPED ov)
{
	BOOL ok = WriteFile(h, buf, size, done, ov);

	API_LOG("WriteFile(%p, %lu) -> %d", h, size, ok);
	return ok;
}

static BOOL WINAPI log_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
	BOOL ok = GetFileSizeEx(h, size);

	API_LOG("GetFileSizeEx(%p) -> %d size=%lld", h, ok,
		ok ? (long long)size->QuadPart : -1LL);
	return ok;
}

static DWORD WINAPI log_GetFileSize(HANDLE h, LPDWORD high)
{
	DWORD n = GetFileSize(h, high);

	API_LOG("GetFileSize(%p) -> %lu", h, n);
	return n;
}

static BOOL WINAPI log_LockFileEx(HANDLE h, DWORD flags, DWORD r, DWORD lo,
				  DWORD hi, LPOVERLAPPED ov)
{
	BOOL ok = LockFileEx(h, flags, r, lo, hi, ov);

	API_LOG("LockFileEx(%p, flags=%lx, len=%lu) -> %d", h, flags, lo, ok);
	return ok;
}

static BOOL WINAPI log_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail)
{
	BOOL ok = CopyFileW(from, to, fail);

	API_LOG("CopyFileW(%ls -> %ls) -> %d", from, to, ok);
	return ok;
}

static BOOL WINAPI log_DeleteFileW(LPCWSTR n)
{
	BOOL ok = DeleteFileW(n);

	API_LOG("DeleteFileW(%ls) -> %d", n, ok);
	return ok;
}

static BOOL WINAPI log_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
	BOOL ok = MoveFileExW(from, to, flags);

	API_LOG("MoveFileExW(%ls -> %ls) -> %d", from, to ? to : L"", ok);
	return ok;
}

static BOOL WINAPI log_SetFilePointerEx(HANDLE h, LARGE_INTEGER d,
					PLARGE_INTEGER pos, DWORD how)
{
	BOOL ok = SetFilePointerEx(h, d, pos, how);

	API_LOG("SetFilePointerEx(%p, %lld, %lu) -> %d", h,
		(long long)d.QuadPart, how, ok);
	return ok;
}

static HDEVINFO WINAPI log_SetupDiGetClassDevsA(const GUID *g, PCSTR e, HWND w,
						DWORD flags)
{
	HDEVINFO h = SetupDiGetClassDevsA(g, e, w, flags);

	API_LOG("SetupDiGetClassDevsA(%08lx-%04x, flags=%lx) -> %p",
		g ? g->Data1 : 0, g ? g->Data2 : 0, flags, h);
	return h;
}

static HDEVINFO WINAPI log_SetupDiGetClassDevsW(const GUID *g, PCWSTR e,
						HWND w, DWORD flags)
{
	HDEVINFO h = SetupDiGetClassDevsW(g, e, w, flags);

	API_LOG("SetupDiGetClassDevsW(%08lx-%04x, flags=%lx) -> %p",
		g ? g->Data1 : 0, g ? g->Data2 : 0, flags, h);
	return h;
}

static BOOL WINAPI log_SetupDiEnumDeviceInterfaces(HDEVINFO h,
						   PSP_DEVINFO_DATA d,
						   const GUID *g, DWORD i,
						   PSP_DEVICE_INTERFACE_DATA o)
{
	BOOL ok = SetupDiEnumDeviceInterfaces(h, d, g, i, o);

	API_LOG("SetupDiEnumDeviceInterfaces(%08lx, #%lu) -> %d",
		g ? g->Data1 : 0, i, ok);
	return ok;
}

static BOOL WINAPI log_SetupDiGetDeviceInterfaceDetailA(
	HDEVINFO h, PSP_DEVICE_INTERFACE_DATA d,
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD size, PDWORD need,
	PSP_DEVINFO_DATA info)
{
	BOOL ok = SetupDiGetDeviceInterfaceDetailA(h, d, detail, size, need,
						   info);

	API_LOG("SetupDiGetDeviceInterfaceDetailA -> %d %s", ok,
		ok && detail ? detail->DevicePath : "");
	return ok;
}

static void *monitored(const char *name)
{
	static const struct { const char *name; void *wrapper; } table[] = {
		{ "SetupDiGetClassDevsA", log_SetupDiGetClassDevsA },
		{ "SetupDiGetClassDevsW", log_SetupDiGetClassDevsW },
		{ "SetupDiEnumDeviceInterfaces", log_SetupDiEnumDeviceInterfaces },
		{ "SetupDiGetDeviceInterfaceDetailA",
		  log_SetupDiGetDeviceInterfaceDetailA },
		{ "ReadFile", log_ReadFile },
		{ "WriteFile", log_WriteFile },
		{ "GetFileSizeEx", log_GetFileSizeEx },
		{ "GetFileSize", log_GetFileSize },
		{ "LockFileEx", log_LockFileEx },
		{ "CopyFileW", log_CopyFileW },
		{ "DeleteFileW", log_DeleteFileW },
		{ "MoveFileExW", log_MoveFileExW },
		{ "SetFilePointerEx", log_SetFilePointerEx },
		{ "GetSystemDirectoryW", log_GetSystemDirectoryW },
		{ "GetModuleHandleW", log_GetModuleHandleW },
		{ "GetModuleFileNameW", log_GetModuleFileNameW },
		{ "GetEnvironmentVariableW", log_GetEnvironmentVariableW },
		{ "OpenEventW", log_OpenEventW },
		{ "CreateFileW", log_CreateFileW },
		{ "GetFileAttributesW", log_GetFileAttributesW },
		{ "CreateFileMappingW", log_CreateFileMappingW },
		{ "OpenFileMappingW", log_OpenFileMappingW },
		{ "CreateMutexW", log_CreateMutexW },
		{ "OpenMutexW", log_OpenMutexW },
		{ "CreateEventW", log_CreateEventW },
		{ "RegOpenKeyExW", log_RegOpenKeyExW },
		{ "RegQueryValueExW", log_RegQueryValueExW },
		{ "DeviceIoControl", log_DeviceIoControl },
	};
	size_t i;

	for (i = 0; i < sizeof(table) / sizeof(*table); i++)
		if (!strcmp(table[i].name, name))
			return table[i].wrapper;
	return NULL;
}

static FARPROC WINAPI global_GetProcAddress(HMODULE module, LPCSTR name)
{
	ANSI_STRING s;
	void *proc = NULL;
	NTSTATUS status;

	if (trace && (uintptr_t)name > 0xffff &&
	    strncmp(name, "D3DKMT", 6) && strncmp(name, "NtGdiDdDDI", 10) &&
	    from_driver(__builtin_return_address(0))) {
		void *w = monitored(name);

		fprintf(stderr, "[probe] driver resolves %s%s\n", name,
			w ? " (monitored)" : "");
		if (w)
			return (FARPROC)w;
	}
	if (!module)
		module = GetModuleHandleW(NULL);
	if ((uintptr_t)name > 0xffff) {
		void *display = kmt_display_resolve(name);

		if (display)
			return (FARPROC)display;
		if (!strncmp(name, "D3DKMT", 6) ||
		    !strncmp(name, "NtGdiDdDDI", 10)) {
			void *f = bridged(name);

			if (trace)
				fprintf(stderr, "[probe] resolve %s -> %s\n",
					name, f ? "bridge" : "Wine");
			if (f)
				return (FARPROC)f;
		}
		s.Buffer = (char *)name;
		s.Length = (USHORT)strlen(name);
		s.MaximumLength = s.Length + 1;
		status = ldr_get_procedure(module, &s, 0, &proc);
	} else {
		status = ldr_get_procedure(module, NULL, (ULONG)(uintptr_t)name,
					   &proc);
	}
	if (status) {
		SetLastError(nt_status_to_dos(status));
		return NULL;
	}
	return (FARPROC)proc;
}

/* Overwrites the start of a function with an absolute jump. */
static void redirect(void *function, void *to)
{
	unsigned char jump[12] = { 0x48, 0xb8 };	/* mov rax, imm64 */
	DWORD old;

	memcpy(jump + 2, &to, sizeof(to));
	jump[10] = 0xff;				/* jmp rax */
	jump[11] = 0xe0;
	VirtualProtect(function, sizeof(jump), PAGE_EXECUTE_READWRITE, &old);
	memcpy(function, jump, sizeof(jump));
	VirtualProtect(function, sizeof(jump), old, &old);
	FlushInstructionCache(GetCurrentProcess(), function, sizeof(jump));
}

/*
 * One-shot breakpoints for reverse engineering: NVPROBE_BREAKS lists
 * addresses in the driver's preferred image (base 0x180000000). Each hit
 * logs RAX (the value a call just returned) and resumes.
 */
#define DRIVER_PREFERRED_BASE 0x180000000ull

struct breakpoint {
	BYTE *address;
	BYTE saved;
	uint64_t file_va;
};
static struct breakpoint breakpoints[32];
static int breakpoint_count;

static LONG CALLBACK on_breakpoint(EXCEPTION_POINTERS *info)
{
	BYTE *at = info->ExceptionRecord->ExceptionAddress;
	DWORD old;
	int i;

	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
		return EXCEPTION_CONTINUE_SEARCH;
	for (i = 0; i < breakpoint_count; i++) {
		struct breakpoint *b = &breakpoints[i];

		if (b->address != at)
			continue;
		fprintf(stderr, "[brk] 0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx\n",
			(unsigned long long)b->file_va,
			(unsigned long long)info->ContextRecord->Rax,
			(unsigned long long)info->ContextRecord->Rcx,
			(unsigned long long)info->ContextRecord->Rdx);
		VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old);
		*at = b->saved;
		VirtualProtect(at, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), at, 1);
		info->ContextRecord->Rip = (DWORD64)(uintptr_t)at;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_breakpoints(BYTE *base)
{
	char list[1024], *p, *end;
	DWORD old;

	if (!GetEnvironmentVariableA("NVPROBE_BREAKS", list, sizeof(list)))
		return;
	AddVectoredExceptionHandler(1, on_breakpoint);
	for (p = list; *p && breakpoint_count < 32; p = end) {
		uint64_t va = strtoull(p, &end, 16);
		struct breakpoint *b;

		if (end == p)
			break;
		while (*end == ',' || *end == ' ')
			end++;
		b = &breakpoints[breakpoint_count++];
		b->file_va = va;
		b->address = base + (va - DRIVER_PREFERRED_BASE);
		VirtualProtect(b->address, 1, PAGE_EXECUTE_READWRITE, &old);
		b->saved = *b->address;
		*b->address = 0xcc;
		VirtualProtect(b->address, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), b->address, 1);
	}
	fprintf(stderr, "[brk] armed %d breakpoints\n", breakpoint_count);
}

/*
 * Load notifications arrive after a module's imports are bound and before
 * its DllMain, the only point where its import table can be routed in time.
 */
struct ldr_loaded {
	ULONG flags;
	const UNICODE_STRING *full_name;
	const UNICODE_STRING *base_name;
	void *base;
	ULONG size;
};

static VOID CALLBACK on_dll_loaded(ULONG reason, const void *data, void *ctx)
{
	const struct ldr_loaded *d = data;
	size_t n = wcslen(driver_dir);

	(void)ctx;
	if (!d->full_name || d->full_name->Length / sizeof(wchar_t) < n ||
	    _wcsnicmp(d->full_name->Buffer, driver_dir, n))
		return;
	if (reason == 2) {
		/* Unloaded: a later load may reuse the address. */
		int i;

		for (i = 0; i < patched_count; i++)
			if (patched[i] == (HMODULE)d->base)
				patched[i] = patched[--patched_count];
		return;
	}
	if (d->base_name && d->base_name->Length >= 24 &&
	    !_wcsnicmp(d->base_name->Buffer, L"nvoglv64.dll", 12))
		arm_breakpoints(d->base);
	patch_module((HMODULE)d->base);
}

static void install_global_hooks(void)
{
	NTSTATUS(WINAPI *register_notification)(ULONG, void *, void *, void **) =
		(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
				       "LdrRegisterDllNotification");
	void *cookie;

	if (register_notification)
		register_notification(0, on_dll_loaded, NULL, &cookie);
	static const wchar_t *with_getprocaddress[] = { L"kernelbase.dll",
							L"kernel32.dll" };
	static const wchar_t *with_d3dkmt[] = { L"gdi32.dll", L"win32u.dll" };
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	void *done[8];
	int n = 0, i, j;

	ldr_get_procedure = (void *)real_GetProcAddress(ntdll, "LdrGetProcedureAddress");
	nt_status_to_dos = (void *)real_GetProcAddress(ntdll, "RtlNtStatusToDosError");

	/* Wine's own D3DKMT entry points (static imports land here). */
	for (i = 0; i < 2; i++) {
		HMODULE m = LoadLibraryW(with_d3dkmt[i]);
		IMAGE_NT_HEADERS64 *nt = (void *)((BYTE *)m +
			((IMAGE_DOS_HEADER *)m)->e_lfanew);
		IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[
			IMAGE_DIRECTORY_ENTRY_EXPORT];
		IMAGE_EXPORT_DIRECTORY *exp = (void *)((BYTE *)m + dir.VirtualAddress);
		DWORD *names = (void *)((BYTE *)m + exp->AddressOfNames);

		for (j = 0; j < (int)exp->NumberOfNames; j++) {
			const char *name = (const char *)m + names[j];
			void *bridge, *f;

			if (strncmp(name, "D3DKMT", 6) &&
			    strncmp(name, "NtGdiDdDDI", 10))
				continue;
			bridge = bridged(name);
			f = (void *)real_GetProcAddress(m, name);
			if (bridge && f)
				redirect(f, bridge);
		}
	}
	/* GetProcAddress itself, wherever Wine implements it. */
	for (i = 0; i < 2; i++) {
		void *f = (void *)real_GetProcAddress(
			GetModuleHandleW(with_getprocaddress[i]), "GetProcAddress");
		int seen = 0;

		for (j = 0; j < n; j++)
			seen |= done[j] == f;
		if (f && !seen) {
			redirect(f, global_GetProcAddress);
			done[n++] = f;
		}
	}
}

/* --- Finding the driver ------------------------------------------------ */

/*
 * The driver checks that it runs from the driver store path dxgkrnl
 * reports, so it is loaded by its host path. The Wine prefix maps
 * C:\Windows\System32\DriverStore\FileRepository to the guest's copy of the
 * host driver store (/usr/lib/wsl/drivers under WSL).
 */
static int guest_driver_path(const wchar_t *host, wchar_t *out)
{
	if (!wcsstr(host, L"\\FileRepository\\") &&
	    !wcsstr(host, L"\\filerepository\\"))
		return -1;
	wcscpy(out, host);
	return 0;
}

/* The NVIDIA adapter, as the Windows Vulkan loader would report it. */
static LUID driver_luid;

static int find_driver(wchar_t *path)
{
	struct d3dkmt_adapterinfo adapters[16];
	struct {
		uint32_t count, reserved;
		uint64_t adapters;
	} e = {16, 0, (uint64_t)(uintptr_t)adapters};
	HMODULE gdi32 = LoadLibraryW(L"gdi32.dll");
	int32_t(WINAPI *enum2)(void *) = native ?
		(void *)GetProcAddress(gdi32, "D3DKMTEnumAdapters2") :
		kmt_bridge_lookup("D3DKMTEnumAdapters2");
	int32_t(WINAPI *query)(void *) = native ?
		(void *)GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo") :
		kmt_bridge_lookup("D3DKMTQueryAdapterInfo");
	int32_t status = enum2(&e);
	uint32_t i;

	printf("D3DKMTEnumAdapters2: 0x%08x, %u adapter(s)\n", (unsigned)status,
	       e.count);
	for (i = 0; status >= 0 && i < e.count; i++) {
		struct {
			wchar_t icd[MAX_PATH];
			ULONG version, flags;
		} gl = {0};
		struct d3dkmt_queryadapterinfo q = {0};

		q.adapter = adapters[i].adapter_handle;
		q.type = 2;	/* KMTQAITYPE_UMOPENGLINFO */
		q.private_data = (uint64_t)(uintptr_t)&gl;
		q.private_data_size = sizeof(gl);
		status = query(&q);
		printf("  adapter %u LUID %08x:%08x OpenGL/Vulkan ICD: %ls\n",
		       i, adapters[i].adapter_luid.b, adapters[i].adapter_luid.a,
		       status >= 0 ? gl.icd : L"(query failed)");
		if (status >= 0 && !*path && wcsstr(gl.icd, L"nvoglv64") &&
		    !guest_driver_path(gl.icd, path)) {
			driver_luid.LowPart = adapters[i].adapter_luid.a;
			driver_luid.HighPart = (LONG)adapters[i].adapter_luid.b;
			continue;
		}
		status = 0;
	}
	return *path ? 0 : -1;
}

/* --- Vulkan ------------------------------------------------------------ */

#define VKCHECK(call)							\
	do {								\
		VkResult r_ = (call);					\
		if (r_ != VK_SUCCESS) {					\
			printf("FAILED %s: VkResult %d\n", #call, r_);	\
			return 1;					\
		}							\
	} while (0)

static double now_ms(void)
{
	LARGE_INTEGER t, f;

	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint32_t expected(uint32_t i)
{
	uint32_t x = i * 2654435761u;
	int k;

	for (k = 0; k < 64; k++)
		x = x * 1664525u + 1013904223u;
	return x;
}

static int run_vulkan(HMODULE icd)
{
	PFN_vkGetInstanceProcAddr gipa =
		(PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
	VkResult(VKAPI_PTR *negotiate)(uint32_t *) =
		(void *)GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
	uint32_t version = 7, count = 0, i, family = UINT32_MAX;
	const uint32_t n = 1u << 22;	/* 4M invocations, 16 MiB */
	VkInstance instance;
	VkPhysicalDevice gpus[8];
	VkDevice device;
	VkQueue queue;
	double t0;

	if (!gipa || !negotiate) {
		printf("FAILED: the DLL exports no Vulkan ICD interface\n");
		return 1;
	}
	VKCHECK(negotiate(&version));
	printf("ICD interface version %u\n", version);

	PFN_vkCreateInstance vkCreateInstance =
		(PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

	app.pApplicationName = "nvvkprobe";
	app.apiVersion = VK_API_VERSION_1_3;
	ici.pApplicationInfo = &app;
	t0 = now_ms();
	VKCHECK(vkCreateInstance(&ici, NULL, &instance));
	printf("vkCreateInstance: %.1f ms\n", now_ms() - t0);

#define IPROC(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
	IPROC(vkEnumeratePhysicalDevices);
	IPROC(vkGetPhysicalDeviceProperties2);
	IPROC(vkGetPhysicalDeviceFeatures2);
	IPROC(vkEnumerateDeviceExtensionProperties);
	IPROC(vkGetPhysicalDeviceQueueFamilyProperties);
	IPROC(vkGetPhysicalDeviceMemoryProperties);
	IPROC(vkCreateDevice);
	IPROC(vkGetDeviceProcAddr);
	IPROC(vkDestroyInstance);

	/*
	 * On Windows the loader enumerates adapters and passes each LUID to
	 * the ICD (interface version 6+); the probe plays the loader.
	 */
	{
		VkResult(VKAPI_PTR *by_adapter)(VkInstance, LUID, uint32_t *,
						VkPhysicalDevice *) =
			(void *)GetProcAddress(icd,
				"vk_icdEnumerateAdapterPhysicalDevices");

		count = 8;
		t0 = now_ms();
		if (by_adapter)
			VKCHECK(by_adapter(instance, driver_luid, &count, gpus));
		else
			VKCHECK(vkEnumeratePhysicalDevices(instance, &count, gpus));
		printf("%s: %u device(s), %.1f ms\n", by_adapter ?
		       "vk_icdEnumerateAdapterPhysicalDevices" :
		       "vkEnumeratePhysicalDevices", count, now_ms() - t0);
	}
	if (!count)
		return 1;

	VkPhysicalDeviceDriverProperties driver = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 props = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
	vkGetPhysicalDeviceProperties2(gpus[0], &props);
	printf("GPU: %s, Vulkan %u.%u.%u, driver %s %s\n",
	       props.properties.deviceName,
	       VK_API_VERSION_MAJOR(props.properties.apiVersion),
	       VK_API_VERSION_MINOR(props.properties.apiVersion),
	       VK_API_VERSION_PATCH(props.properties.apiVersion),
	       driver.driverName, driver.driverInfo);

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rt };
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, &as };
	VkPhysicalDeviceFeatures2 features = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &mesh };
	vkGetPhysicalDeviceFeatures2(gpus[0], &features);
	printf("Ray tracing pipeline: %s, acceleration structures: %s, "
	       "mesh shaders: %s\n", rt.rayTracingPipeline ? "yes" : "no",
	       as.accelerationStructure ? "yes" : "no",
	       mesh.meshShader ? "yes" : "no");

	{
		static const char *wanted[] = {
			"VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_query",
			"VK_NVX_binary_import", "VK_NVX_image_view_handle",
			"VK_NV_low_latency2", "VK_NV_optical_flow",
			"VK_KHR_video_decode_queue", "VK_KHR_video_encode_queue",
			"VK_KHR_external_memory_win32", "VK_KHR_swapchain",
		};
		VkExtensionProperties *ext;
		uint32_t n_ext = 0, w;

		vkEnumerateDeviceExtensionProperties(gpus[0], NULL, &n_ext, NULL);
		ext = calloc(n_ext, sizeof(*ext));
		vkEnumerateDeviceExtensionProperties(gpus[0], NULL, &n_ext, ext);
		printf("%u device extensions; key ones:", n_ext);
		for (w = 0; w < sizeof(wanted) / sizeof(*wanted); w++) {
			int found = 0;

			for (i = 0; i < n_ext; i++)
				found |= !strcmp(ext[i].extensionName, wanted[w]);
			printf(" %s%s", found ? "+" : "-", wanted[w] + 3);
		}
		printf("\n");
		free(ext);
	}

	{
		VkQueueFamilyProperties qf[16];
		uint32_t nqf = 16;

		vkGetPhysicalDeviceQueueFamilyProperties(gpus[0], &nqf, qf);
		for (i = 0; i < nqf && family == UINT32_MAX; i++)
			if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
				family = i;
	}
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	t0 = now_ms();
	VKCHECK(vkCreateDevice(gpus[0], &dci, NULL, &device));
	printf("vkCreateDevice: %.1f ms\n", now_ms() - t0);

#define DPROC(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
	DPROC(vkGetDeviceQueue);
	DPROC(vkCreateBuffer);
	DPROC(vkGetBufferMemoryRequirements);
	DPROC(vkAllocateMemory);
	DPROC(vkBindBufferMemory);
	DPROC(vkMapMemory);
	DPROC(vkCreateShaderModule);
	DPROC(vkCreateDescriptorSetLayout);
	DPROC(vkCreatePipelineLayout);
	DPROC(vkCreateComputePipelines);
	DPROC(vkCreateDescriptorPool);
	DPROC(vkAllocateDescriptorSets);
	DPROC(vkUpdateDescriptorSets);
	DPROC(vkCreateCommandPool);
	DPROC(vkAllocateCommandBuffers);
	DPROC(vkBeginCommandBuffer);
	DPROC(vkCmdBindPipeline);
	DPROC(vkCmdBindDescriptorSets);
	DPROC(vkCmdDispatch);
	DPROC(vkEndCommandBuffer);
	DPROC(vkCreateFence);
	DPROC(vkResetFences);
	DPROC(vkQueueSubmit);
	DPROC(vkWaitForFences);
	DPROC(vkDeviceWaitIdle);
	DPROC(vkDestroyDevice);
	DPROC(vkCreateQueryPool);
	DPROC(vkCmdResetQueryPool);
	DPROC(vkCmdWriteTimestamp);
	DPROC(vkGetQueryPoolResults);
	vkGetDeviceQueue(device, family, 0, &queue);

	/* A host-visible, preferably cached buffer the shader fills. */
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkBuffer buffer;
	VkMemoryRequirements req;
	VkPhysicalDeviceMemoryProperties mp;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkDeviceMemory memory;
	uint32_t *data;
	int best = -1;

	bci.size = (VkDeviceSize)n * 4;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VKCHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
	vkGetBufferMemoryRequirements(device, buffer, &req);
	vkGetPhysicalDeviceMemoryProperties(gpus[0], &mp);
	for (i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;

		if (!(req.memoryTypeBits & (1u << i)) ||
		    !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
		    !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			continue;
		if (best < 0 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
			best = (int)i;
	}
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = (uint32_t)best;
	VKCHECK(vkAllocateMemory(device, &mai, NULL, &memory));
	VKCHECK(vkBindBufferMemory(device, buffer, memory, 0));
	VKCHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data));
	memset(data, 0, (size_t)n * 4);

	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule shader;
	smci.codeSize = sizeof(probe_compute_spv);
	smci.pCode = probe_compute_spv;
	VKCHECK(vkCreateShaderModule(device, &smci, NULL, &shader));

	VkDescriptorSetLayoutBinding binding = { 0,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayout dsl;
	dslci.bindingCount = 1;
	dslci.pBindings = &binding;
	VKCHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));

	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	VKCHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));

	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkPipeline pipeline;
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = shader;
	cpci.stage.pName = "main";
	cpci.layout = layout;
	t0 = now_ms();
	VKCHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
					 &pipeline));
	printf("compute pipeline (shader compiled in the guest): %.1f ms\n",
	       now_ms() - t0);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorPool pool;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VKCHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkDescriptorSet set;
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &dsl;
	VKCHECK(vkAllocateDescriptorSets(device, &dsai, &set));
	VkDescriptorBufferInfo dbi = { buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	wds.dstSet = set;
	wds.descriptorCount = 1;
	wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	wds.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(device, 1, &wds, 0, NULL);

	VkCommandPoolCreateInfo cpoolci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool cpool;
	cpoolci.queueFamilyIndex = family;
	VKCHECK(vkCreateCommandPool(device, &cpoolci, NULL, &cpool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBuffer cb;
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VKCHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
	/* Timestamps around the dispatch give the GPU's own execution time. */
	VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	VkQueryPool timestamps;
	qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qpci.queryCount = 2;
	VKCHECK(vkCreateQueryPool(device, &qpci, NULL, &timestamps));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	VKCHECK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdResetQueryPool(cb, timestamps, 0, 2);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
				&set, 0, NULL);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
	vkCmdDispatch(cb, n / 64, 1, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
	VKCHECK(vkEndCommandBuffer(cb));

	/* An almost empty command buffer: what one submission costs. */
	VkCommandBuffer tiny;
	VKCHECK(vkAllocateCommandBuffers(device, &cbai, &tiny));
	VKCHECK(vkBeginCommandBuffer(tiny, &cbbi));
	vkCmdBindPipeline(tiny, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(tiny, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
				1, &set, 0, NULL);
	vkCmdDispatch(tiny, 1, 1, 1);
	VKCHECK(vkEndCommandBuffer(tiny));

	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	double best_ms = 1e9;
	int round;

	VKCHECK(vkCreateFence(device, &fci, NULL, &fence));
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	for (round = 0; round < 20; round++) {
		double t;

		VKCHECK(vkResetFences(device, 1, &fence));
		t = now_ms();
		VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		t = now_ms() - t;
		if (t < best_ms)
			best_ms = t;
	}
	printf("dispatch of %u invocations x 64 iterations: best %.3f ms "
	       "submit-to-signal over 20 rounds\n", n, best_ms);

	{
		uint64_t ts[2];
		double gpu_ms, total;
		const int batch = 200, submits = 2000;

		VKCHECK(vkGetQueryPoolResults(device, timestamps, 0, 2,
					      sizeof(ts), ts, sizeof(ts[0]),
					      VK_QUERY_RESULT_64_BIT |
					      VK_QUERY_RESULT_WAIT_BIT));
		gpu_ms = (double)(ts[1] - ts[0]) *
			 props.properties.limits.timestampPeriod / 1e6;
		printf("GPU execution time of that dispatch: %.3f ms\n", gpu_ms);

		/* Back to back, as frames are: submission overlaps the GPU. */
		VKCHECK(vkResetFences(device, 1, &fence));
		total = now_ms();
		for (round = 0; round < batch; round++)
			VKCHECK(vkQueueSubmit(queue, 1, &si,
					      round == batch - 1 ? fence :
					      VK_NULL_HANDLE));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		total = now_ms() - total;
		printf("pipelined: %d dispatches in %.1f ms, %.3f ms each "
		       "(GPU-bound: %.0f%% of GPU time)\n", batch, total,
		       total / batch, 100.0 * gpu_ms * batch / total);

		si.pCommandBuffers = &tiny;
		VKCHECK(vkResetFences(device, 1, &fence));
		total = now_ms();
		for (round = 0; round < submits; round++)
			VKCHECK(vkQueueSubmit(queue, 1, &si,
					      round == submits - 1 ? fence :
					      VK_NULL_HANDLE));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		total = now_ms() - total;
		printf("submission rate: %d submits in %.1f ms, %.1f us each, "
		       "%.0f per second\n", submits, total,
		       total * 1000.0 / submits, submits * 1000.0 / total);
		si.pCommandBuffers = &cb;
	}

	{
		uint32_t bad = 0;

		for (i = 0; i < n; i++)
			bad += data[i] != expected(i);
		printf("compute result: %u of %u values correct%s\n", n - bad, n,
		       bad ? "  <-- MISMATCH" : "");
		vkDeviceWaitIdle(device);
		vkDestroyDevice(device, NULL);
		vkDestroyInstance(instance, NULL);
		return bad ? 1 : 0;
	}
}

int main(int argc, char **argv)
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	wchar_t path[MAX_PATH] = L"";
	HMODULE icd;
	int i, ret;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trace"))
			trace = 1;
		else if (!strcmp(argv[i], "--stacks"))
			trace = 2;
		else if (!strcmp(argv[i], "--native"))
			native = 1;
		else
			MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, path, MAX_PATH);
	}
	real_GetProcAddress = (void *)GetProcAddress(kernel32, "GetProcAddress");
	real_LoadLibraryA = (void *)GetProcAddress(kernel32, "LoadLibraryA");
	real_LoadLibraryW = (void *)GetProcAddress(kernel32, "LoadLibraryW");
	real_LoadLibraryExA = (void *)GetProcAddress(kernel32, "LoadLibraryExA");
	real_LoadLibraryExW = (void *)GetProcAddress(kernel32, "LoadLibraryExW");

	if (!native) {
		wchar_t so[MAX_PATH];

		GetModuleFileNameW(NULL, so, MAX_PATH);
		wcscpy(wcsrchr(so, L'\\') + 1, L"dxgkmt.so");
		ret = kmt_bridge_open(so, trace);
		if (ret) {
			printf("FAILED: cannot open /dev/dxg through %ls "
			       "(errno %d)\n", so, -ret);
			return 1;
		}
	}
	if (!*path && find_driver(path)) {
		printf("FAILED: no NVIDIA ICD reported by the adapter\n");
		return 1;
	}
	wcscpy(driver_dir, path);
	*wcsrchr(driver_dir, L'\\') = 0;
	if (!native)
		install_global_hooks();
	printf("loading %ls\n", path);
	icd = LoadLibraryW(path);
	if (!icd) {
		printf("FAILED: LoadLibrary error %lu\n", GetLastError());
		return 1;
	}
	if (!native)
		patch_module(icd);
	ret = run_vulkan(icd);
	if (!native)
		kmt_bridge_report();
	printf("%s\n", ret ? "PROBE FAILED" : "PROBE PASSED");
	return ret;
}
