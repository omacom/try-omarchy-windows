/* threadsets PID [apply]: the threads of a process with their names and CPU
 * time; with "apply", the threads whose name contains "CPU" (QEMU's vCPU
 * threads) get the performance cores as their selected CPU sets (a soft
 * affinity the scheduler keeps to when it can) and the other threads the
 * efficiency cores, so helper threads do not take a core from a vCPU.
 * Build (MSYS2): gcc -O2 threadsets.c -o threadsets.exe */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR *);

int main(int argc, char **argv)
{
	if (argc < 2)
		return 1;
	DWORD pid = (DWORD)atoi(argv[1]);
	int apply = argc > 2 && !strcmp(argv[2], "apply");
	int reset = argc > 2 && !strcmp(argv[2], "reset");
	/* "vcpu": only the vCPU threads get the performance cores. */
	int vcpu_only = argc > 2 && !strcmp(argv[2], "vcpu");
	apply |= vcpu_only;
	get_desc_fn get_desc = (get_desc_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

	/* CPU set ids by efficiency class. */
	ULONG len = 0;
	GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
	char *buf = malloc(len);
	GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0);
	ULONG pset[64], eset[64];
	int np = 0, ne = 0, maxclass = 0;
	for (ULONG off = 0; off < len;) {
		PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
		if (s->CpuSet.EfficiencyClass > maxclass)
			maxclass = s->CpuSet.EfficiencyClass;
		off += s->Size;
	}
	for (ULONG off = 0; off < len;) {
		PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
		if (s->CpuSet.EfficiencyClass == maxclass)
			pset[np++] = s->CpuSet.Id;
		else
			eset[ne++] = s->CpuSet.Id;
		off += s->Size;
	}
	printf("performance cpu sets %d, efficiency %d\n", np, ne);

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te = { .dwSize = sizeof(te) };
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid)
			continue;
		HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE,
				      te.th32ThreadID);
		if (!h)
			continue;
		PWSTR name = NULL;
		if (get_desc)
			get_desc(h, &name);
		FILETIME c, e, k, u;
		GetThreadTimes(h, &c, &e, &k, &u);
		double secs = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
			       ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7;
		const char *action = "";
		if (apply || reset) {
			BOOL vcpu = name && wcsstr(name, L"CPU");
			BOOL r;
			if (reset || (vcpu_only && !vcpu))
				r = SetThreadSelectedCpuSets(h, NULL, 0);
			else if (vcpu)
				r = SetThreadSelectedCpuSets(h, pset, np);
			else
				r = SetThreadSelectedCpuSets(h, eset, ne);
			action = !r ? " (failed)" : reset ? " -> any" : vcpu ? " -> P" : " -> E";
		}
		printf("%6lu %8.2f s  %ls%s\n", te.th32ThreadID, secs, name && *name ? name : L"(unnamed)", action);
		if (name)
			LocalFree(name);
		CloseHandle(h);
	}
	CloseHandle(snap);
	return 0;
}
