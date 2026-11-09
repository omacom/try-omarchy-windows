// SPDX-License-Identifier: MIT
/*
 * CUDA through /dev/dxg with NVIDIA's dxgkrnl user-mode driver (the
 * libcuda.so.1 Windows installs for WSL). No CUDA toolkit: the driver API
 * is loaded with dlopen and a PTX kernel is compiled by the driver's JIT.
 * Checks a vector add on 16M floats and measures copy bandwidth.
 *
 *   gcc -O2 -o cudatest cudatest.c -ldl
 *   LD_LIBRARY_PATH=/usr/lib/wsl/lib ./cudatest
 */
#ifdef _WIN32
/* The same test on the Windows host, for a native baseline. */
#include <windows.h>
#define dlopen(name, flags) ((void *)LoadLibraryA("nvcuda.dll"))
#define dlsym(lib, sym) ((void *)GetProcAddress((HMODULE)(lib), (sym)))
#define dlerror() "LoadLibrary(nvcuda.dll) failed"
#else
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext, *CUmodule, *CUfunction;
typedef unsigned long long CUdeviceptr;

static const char ptx[] =
	".version 7.0\n"
	".target sm_52\n"
	".address_size 64\n"
	".visible .entry vadd(.param .u64 pa, .param .u64 pb, .param .u64 pc,\n"
	"                     .param .u32 pn)\n"
	"{\n"
	"	.reg .pred %p<2>;\n"
	"	.reg .b32 %r<6>;\n"
	"	.reg .f32 %f<4>;\n"
	"	.reg .b64 %rd<11>;\n"
	"	ld.param.u64 %rd1, [pa];\n"
	"	ld.param.u64 %rd2, [pb];\n"
	"	ld.param.u64 %rd3, [pc];\n"
	"	ld.param.u32 %r2, [pn];\n"
	"	mov.u32 %r3, %ctaid.x;\n"
	"	mov.u32 %r4, %ntid.x;\n"
	"	mov.u32 %r5, %tid.x;\n"
	"	mad.lo.s32 %r1, %r3, %r4, %r5;\n"
	"	setp.ge.s32 %p1, %r1, %r2;\n"
	"	@%p1 bra DONE;\n"
	"	cvta.to.global.u64 %rd4, %rd1;\n"
	"	cvta.to.global.u64 %rd5, %rd2;\n"
	"	cvta.to.global.u64 %rd6, %rd3;\n"
	"	mul.wide.s32 %rd7, %r1, 4;\n"
	"	add.s64 %rd8, %rd4, %rd7;\n"
	"	add.s64 %rd9, %rd5, %rd7;\n"
	"	add.s64 %rd10, %rd6, %rd7;\n"
	"	ld.global.f32 %f1, [%rd8];\n"
	"	ld.global.f32 %f2, [%rd9];\n"
	"	add.f32 %f3, %f1, %f2;\n"
	"	st.global.f32 [%rd10], %f3;\n"
	"DONE:\n"
	"	ret;\n"
	"}\n";

#define FN(ret, name, args) static ret(*name) args
FN(CUresult, cuInit, (unsigned));
FN(CUresult, cuDriverGetVersion, (int *));
FN(CUresult, cuDeviceGetCount, (int *));
FN(CUresult, cuDeviceGet, (CUdevice *, int));
FN(CUresult, cuDeviceGetName, (char *, int, CUdevice));
FN(CUresult, cuDeviceTotalMem_v2, (size_t *, CUdevice));
FN(CUresult, cuDeviceGetAttribute, (int *, int, CUdevice));
FN(CUresult, cuDevicePrimaryCtxRetain, (CUcontext *, CUdevice));
FN(CUresult, cuCtxSetCurrent, (CUcontext));
FN(CUresult, cuCtxSynchronize, (void));
FN(CUresult, cuModuleLoadData, (CUmodule *, const void *));
FN(CUresult, cuModuleGetFunction, (CUfunction *, CUmodule, const char *));
FN(CUresult, cuMemAlloc_v2, (CUdeviceptr *, size_t));
FN(CUresult, cuMemcpyHtoD_v2, (CUdeviceptr, const void *, size_t));
FN(CUresult, cuMemcpyDtoH_v2, (void *, CUdeviceptr, size_t));
FN(CUresult, cuMemAllocHost_v2, (void **, size_t));
FN(CUresult, cuLaunchKernel, (CUfunction, unsigned, unsigned, unsigned,
			      unsigned, unsigned, unsigned, unsigned, void *,
			      void **, void **));
FN(CUresult, cuGetErrorString, (CUresult, const char **));

static int failed(const char *what, CUresult r)
{
	const char *s = "?";

	if (!r)
		return 0;
	if (cuGetErrorString)
		cuGetErrorString(r, &s);
	printf("FAIL %s: %d %s\n", what, r, s);
	return 1;
}

#define CK(call) do { if (failed(#call, call)) return 1; } while (0)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
	void *lib = dlopen("libcuda.so.1", RTLD_NOW);
	const unsigned n = 16u << 20;
	size_t bytes = (size_t)n * 4, total;
	CUdeviceptr a, b, c;
	CUcontext ctx;
	CUmodule mod;
	CUfunction fn;
	CUdevice dev;
	float *ha, *hb, *hc;
	char name[256];
	int count, version, major, minor, sms, bad = 0;
	unsigned i, nn = n;
	double t;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!lib) {
		printf("FAIL dlopen libcuda.so.1: %s\n", dlerror());
		return 1;
	}
#define LOAD(f) f = dlsym(lib, #f); if (!f) { printf("FAIL no %s\n", #f); return 1; }
	LOAD(cuGetErrorString) LOAD(cuInit) LOAD(cuDriverGetVersion)
	LOAD(cuDeviceGetCount) LOAD(cuDeviceGet) LOAD(cuDeviceGetName)
	LOAD(cuDeviceTotalMem_v2) LOAD(cuDeviceGetAttribute)
	LOAD(cuDevicePrimaryCtxRetain) LOAD(cuCtxSetCurrent)
	LOAD(cuCtxSynchronize) LOAD(cuModuleLoadData) LOAD(cuModuleGetFunction)
	LOAD(cuMemAlloc_v2) LOAD(cuMemcpyHtoD_v2) LOAD(cuMemcpyDtoH_v2)
	LOAD(cuMemAllocHost_v2) LOAD(cuLaunchKernel)

	t = now_ms();
	CK(cuInit(0));
	printf("cuInit: %.1f ms\n", now_ms() - t);
	CK(cuDriverGetVersion(&version));
	CK(cuDeviceGetCount(&count));
	CK(cuDeviceGet(&dev, 0));
	CK(cuDeviceGetName(name, sizeof(name), dev));
	CK(cuDeviceTotalMem_v2(&total, dev));
	CK(cuDeviceGetAttribute(&major, 75, dev));	/* compute capability */
	CK(cuDeviceGetAttribute(&minor, 76, dev));
	CK(cuDeviceGetAttribute(&sms, 16, dev));	/* multiprocessors */
	printf("CUDA %d.%d driver API, %d device(s): %s, sm_%d%d, %d SMs, "
	       "%zu MiB\n", version / 1000, version % 1000 / 10, count, name,
	       major, minor, sms, total >> 20);

	CK(cuDevicePrimaryCtxRetain(&ctx, dev));
	CK(cuCtxSetCurrent(ctx));
	t = now_ms();
	CK(cuModuleLoadData(&mod, ptx));
	CK(cuModuleGetFunction(&fn, mod, "vadd"));
	printf("PTX JIT: %.1f ms\n", now_ms() - t);

	CK(cuMemAllocHost_v2((void **)&ha, bytes));
	CK(cuMemAllocHost_v2((void **)&hb, bytes));
	CK(cuMemAllocHost_v2((void **)&hc, bytes));
	for (i = 0; i < n; i++) {
		ha[i] = (float)i;
		hb[i] = (float)(n - i);
	}
	CK(cuMemAlloc_v2(&a, bytes));
	CK(cuMemAlloc_v2(&b, bytes));
	CK(cuMemAlloc_v2(&c, bytes));
	t = now_ms();
	CK(cuMemcpyHtoD_v2(a, ha, bytes));
	CK(cuMemcpyHtoD_v2(b, hb, bytes));
	t = now_ms() - t;
	printf("host to device: %.1f GB/s (pinned)\n", 2.0 * bytes / t / 1e6);
	{
		void *args[] = { &a, &b, &c, &nn };
		double best = 1e9;
		int r;

		for (r = 0; r < 10; r++) {
			t = now_ms();
			CK(cuLaunchKernel(fn, (n + 255) / 256, 1, 1, 256, 1, 1, 0,
					  NULL, args, NULL));
			CK(cuCtxSynchronize());
			t = now_ms() - t;
			if (t < best)
				best = t;
		}
		printf("vector add of %u floats: best %.3f ms (%.0f GB/s "
		       "effective)\n", n, best, 3.0 * bytes / best / 1e6);
	}
	{
		/* Latency: a one-block kernel, launched and waited for, 1000
		 * times; then 1000 launches with a single wait (throughput). */
		unsigned one = 256;
		void *args[] = { &a, &b, &c, &one };
		double sorted[1000];
		int r, j;

		for (r = 0; r < 1000; r++) {
			t = now_ms();
			CK(cuLaunchKernel(fn, 1, 1, 1, 256, 1, 1, 0, NULL, args,
					  NULL));
			CK(cuCtxSynchronize());
			sorted[r] = now_ms() - t;
		}
		for (r = 1; r < 1000; r++)	/* insertion sort */
			for (j = r; j > 0 && sorted[j - 1] > sorted[j]; j--) {
				double x = sorted[j];

				sorted[j] = sorted[j - 1];
				sorted[j - 1] = x;
			}
		t = now_ms();
		for (r = 0; r < 1000; r++)
			CK(cuLaunchKernel(fn, 1, 1, 1, 256, 1, 1, 0, NULL, args,
					  NULL));
		CK(cuCtxSynchronize());
		t = now_ms() - t;
		printf("launch + sync: median %.1f us, p99 %.1f us; "
		       "1000 launches: %.1f us each\n", sorted[500] * 1e3,
		       sorted[990] * 1e3, t);
	}
	t = now_ms();
	CK(cuMemcpyDtoH_v2(hc, c, bytes));
	t = now_ms() - t;
	printf("device to host: %.1f GB/s (pinned)\n", bytes / t / 1e6);
	for (i = 0; i < n; i++)
		bad += hc[i] != (float)n;
	printf("result: %u of %u values correct\n%s\n", n - bad, n,
	       bad ? "CUDA TEST FAILED" : "CUDA TEST PASSED");
	return bad != 0;
}
