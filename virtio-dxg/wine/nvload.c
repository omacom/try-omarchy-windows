// SPDX-License-Identifier: MIT
/*
 * Loads NVIDIA's Windows Vulkan driver (nvoglv64.dll) under Wine with its
 * kernel calls (D3DKMT) routed to /dev/dxg through kmt_bridge.c. Shared by
 * the probe (nvvkprobe.c) and the Vulkan driver for Wine (dxgvk.c).
 */
#include <windows.h>
#include <winternl.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kmt_bridge.h"
#include "nvload.h"

static int trace;
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

/*
 * Shared-object handles from kmt_bridge_fd_handle are descriptors, not Wine
 * handles: the driver duplicating or closing one must not reach Wine.
 */
static BOOL WINAPI hook_DuplicateHandle(HANDLE sp, HANDLE h, HANDLE tp, HANDLE *out,
					DWORD access, BOOL inherit, DWORD options)
{
	if (kmt_bridge_handle_fd(h) >= 0) {
		if (out)
			*out = h;
		if (trace)
			fprintf(stderr, "[probe] DuplicateHandle of descriptor handle %p\n", h);
		return TRUE;
	}
	return DuplicateHandle(sp, h, tp, out, access, inherit, options);
}

static BOOL WINAPI hook_CloseHandle(HANDLE h)
{
	if (kmt_bridge_handle_fd(h) >= 0)
		return TRUE;
	return CloseHandle(h);
}

static NTSTATUS(WINAPI *real_NtClose)(HANDLE);
static NTSTATUS(WINAPI *real_NtDuplicateObject)(HANDLE, HANDLE, HANDLE, HANDLE *,
						ACCESS_MASK, ULONG, ULONG);

static NTSTATUS WINAPI hook_NtClose(HANDLE h)
{
	if (kmt_bridge_handle_fd(h) >= 0)
		return 0;
	return real_NtClose(h);
}

static NTSTATUS WINAPI hook_NtDuplicateObject(HANDLE sp, HANDLE h, HANDLE tp,
					      HANDLE *out, ACCESS_MASK access,
					      ULONG attributes, ULONG options)
{
	if (kmt_bridge_handle_fd(h) >= 0) {
		if (out)
			*out = h;
		return 0;
	}
	return real_NtDuplicateObject(sp, h, tp, out, access, attributes, options);
}

static void *monitored(const char *name);

static void *replacement(const char *dll, const char *name)
{
	(void)dll;
	if (!strcmp(name, "DuplicateHandle"))
		return hook_DuplicateHandle;
	if (!strcmp(name, "CloseHandle"))
		return hook_CloseHandle;
	if (!strcmp(name, "NtClose") || !strcmp(name, "ZwClose")) {
		if (!real_NtClose)
			real_NtClose = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtClose");
		return hook_NtClose;
	}
	if (!strcmp(name, "NtDuplicateObject") || !strcmp(name, "ZwDuplicateObject")) {
		if (!real_NtDuplicateObject)
			real_NtDuplicateObject = (void *)GetProcAddress(
				GetModuleHandleW(L"ntdll.dll"), "NtDuplicateObject");
		return hook_NtDuplicateObject;
	}
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
	if ((uintptr_t)name > 0xffff &&
	    (!strcmp(name, "DuplicateHandle") || !strcmp(name, "CloseHandle") ||
	     !strcmp(name, "NtClose") || !strcmp(name, "NtDuplicateObject")) &&
	    from_driver(__builtin_return_address(0)))
		return (FARPROC)replacement(NULL, name);
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

int nv_find_driver(wchar_t *path, LUID *luid, int on_windows)
{
	int ret;

	native = on_windows;
	ret = find_driver(path);
	*luid = driver_luid;
	return ret;
}

int nv_load(int trace_level, const wchar_t *unix_lib, HMODULE *icd, LUID *luid)
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	wchar_t path[MAX_PATH] = L"";
	int ret;

	trace = trace_level;
	native = 0;
	real_GetProcAddress = (void *)GetProcAddress(kernel32, "GetProcAddress");
	real_LoadLibraryA = (void *)GetProcAddress(kernel32, "LoadLibraryA");
	real_LoadLibraryW = (void *)GetProcAddress(kernel32, "LoadLibraryW");
	real_LoadLibraryExA = (void *)GetProcAddress(kernel32, "LoadLibraryExA");
	real_LoadLibraryExW = (void *)GetProcAddress(kernel32, "LoadLibraryExW");

	ret = kmt_bridge_open(unix_lib, trace);
	if (ret) {
		fprintf(stderr, "nvload: cannot open /dev/dxg through %ls (errno %d)\n",
			unix_lib, -ret);
		return ret;
	}
	if (find_driver(path)) {
		fprintf(stderr, "nvload: no NVIDIA ICD reported by the adapter\n");
		return -2;
	}
	wcscpy(driver_dir, path);
	*wcsrchr(driver_dir, L'\\') = 0;
	install_global_hooks();
	*icd = LoadLibraryW(path);
	if (!*icd) {
		fprintf(stderr, "nvload: LoadLibrary %ls: error %lu\n", path,
			GetLastError());
		return -3;
	}
	patch_module(*icd);
	*luid = driver_luid;
	return 0;
}
