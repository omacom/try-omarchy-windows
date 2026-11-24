"""d3d12count_gen.py OUT.c: writes d3d12count.c, an LD_PRELOAD library that
counts every method call (by vtable slot) a process makes on its D3D12
device, command lists, command queues and fences. Mesa creates its device
through D3D12GetInterface or D3D12CreateDevice, looked up with dlsym, which
the library wraps; the vtables of those four interfaces are then redirected
through counting thunks (as many slots as the latest interface version
has). SIGUSR1 writes the counts since the last signal to $D3D12COUNT_OUT
(default /tmp/d3d12count.txt) and resets them, with repeats of the same
pipeline state, root signature, vertex buffer, descriptor table, viewport
and scissor.
With D3D12COUNT_TIME=1 every call is also timed (TSC before and after, the
return address kept on a per-thread shadow stack), and the list is sorted
by time spent inside the call: the runtime's and user-mode driver's cost
per method, including what they wait for in the kernel.
Build: gcc -O2 -shared -fPIC d3d12count.c -o d3d12count.so -ldl"""
import sys

CL = ('QueryInterface AddRef Release GetPrivateData SetPrivateData SetPrivateDataInterface SetName '
      'GetDevice GetType Close Reset ClearState DrawInstanced DrawIndexedInstanced Dispatch '
      'CopyBufferRegion CopyTextureRegion CopyResource CopyTiles ResolveSubresource '
      'IASetPrimitiveTopology RSSetViewports RSSetScissorRects OMSetBlendFactor OMSetStencilRef '
      'SetPipelineState ResourceBarrier ExecuteBundle SetDescriptorHeaps SetComputeRootSignature '
      'SetGraphicsRootSignature SetComputeRootDescriptorTable SetGraphicsRootDescriptorTable '
      'SetComputeRoot32BitConstant SetGraphicsRoot32BitConstant SetComputeRoot32BitConstants '
      'SetGraphicsRoot32BitConstants SetComputeRootConstantBufferView SetGraphicsRootConstantBufferView '
      'SetComputeRootShaderResourceView SetGraphicsRootShaderResourceView SetComputeRootUnorderedAccessView '
      'SetGraphicsRootUnorderedAccessView IASetIndexBuffer IASetVertexBuffers SOSetTargets '
      'OMSetRenderTargets ClearDepthStencilView ClearRenderTargetView ClearUnorderedAccessViewUint '
      'ClearUnorderedAccessViewFloat DiscardResource BeginQuery EndQuery ResolveQueryData SetPredication '
      'SetMarker BeginEvent EndEvent ExecuteIndirect AtomicCopyBufferUINT AtomicCopyBufferUINT64 '
      'OMSetDepthBounds SetSamplePositions ResolveSubresourceRegion SetViewInstanceMask '
      'WriteBufferImmediate SetProtectedResourceSession BeginRenderPass EndRenderPass '
      'InitializeMetaCommand ExecuteMetaCommand BuildRaytracingAccelerationStructure '
      'EmitRaytracingAccelerationStructurePostbuildInfo CopyRaytracingAccelerationStructure '
      'SetPipelineState1 DispatchRays RSSetShadingRate RSSetShadingRateImage DispatchMesh Barrier '
      'OMSetFrontAndBackStencilRef RSSetDepthBias IASetIndexBufferStripCutValue SetProgram DispatchGraph').split()
DEV = ('QueryInterface AddRef Release GetPrivateData SetPrivateData SetPrivateDataInterface SetName '
       'GetNodeCount CreateCommandQueue CreateCommandAllocator CreateGraphicsPipelineState '
       'CreateComputePipelineState CreateCommandList CheckFeatureSupport CreateDescriptorHeap '
       'GetDescriptorHandleIncrementSize CreateRootSignature CreateConstantBufferView '
       'CreateShaderResourceView CreateUnorderedAccessView CreateRenderTargetView CreateDepthStencilView '
       'CreateSampler CopyDescriptors CopyDescriptorsSimple GetResourceAllocationInfo '
       'GetCustomHeapProperties CreateCommittedResource CreateHeap CreatePlacedResource '
       'CreateReservedResource CreateSharedHandle OpenSharedHandle OpenSharedHandleByName MakeResident '
       'Evict CreateFence GetDeviceRemovedReason GetCopyableFootprints CreateQueryHeap SetStablePowerState '
       'CreateCommandSignature GetResourceTiling GetAdapterLuid '
       # ID3D12Device1 .. ID3D12Device14
       'CreatePipelineLibrary SetEventOnMultipleFenceCompletion SetResidencyPriority '
       'CreatePipelineState '
       'OpenExistingHeapFromAddress OpenExistingHeapFromFileMapping EnqueueMakeResident '
       'CreateCommandList1 CreateProtectedResourceSession CreateCommittedResource1 CreateHeap1 '
       'CreateReservedResource1 GetResourceAllocationInfo1 '
       'CreateLifetimeTracker RemoveDevice EnumerateMetaCommands EnumerateMetaCommandParameters '
       'CreateMetaCommand CreateStateObject GetRaytracingAccelerationStructurePrebuildInfo '
       'CheckDriverMatchingIdentifier '
       'SetBackgroundProcessingMode '
       'AddToStateObject CreateProtectedResourceSession1 '
       'GetResourceAllocationInfo2 CreateCommittedResource2 CreatePlacedResource1 '
       'CreateSamplerFeedbackUnorderedAccessView GetCopyableFootprints1 '
       'CreateShaderCacheSession ShaderCacheControl CreateCommandQueue1 '
       'CreateCommittedResource3 CreatePlacedResource2 CreateReservedResource2 '
       'CreateSampler2 GetResourceAllocationInfo3 OpenExistingHeapFromAddress1 '
       'CreateRootSignatureFromSubobjectInLibrary').split()
QUEUE = ('QueryInterface AddRef Release GetPrivateData SetPrivateData SetPrivateDataInterface SetName '
         'GetDevice UpdateTileMappings CopyTileMappings ExecuteCommandLists SetMarker BeginEvent EndEvent '
         'Signal Wait GetTimestampFrequency GetClockCalibration GetDesc').split()
FENCE = ('QueryInterface AddRef Release GetPrivateData SetPrivateData SetPrivateDataInterface SetName '
         'GetDevice GetCompletedValue SetEventOnCompletion Signal').split()
# (C prefix, method names, label)
TABLES = [('cl', CL, 'cmdlist'), ('dev', DEV, 'device'), ('q', QUEUE, 'queue'), ('fn', FENCE, 'fence')]

out = ['#define _GNU_SOURCE',
       '#include <dlfcn.h>', '#include <signal.h>', '#include <stdarg.h>', '#include <stdint.h>', '#include <stdio.h>',
       '#include <stdlib.h>', '#include <string.h>', '#include <sys/mman.h>', '#include <time.h>',
       '#include <unistd.h>', '',
       '#define H __attribute__((visibility("hidden")))',
       'static uint64_t rep_pso, rep_rs, rep_vb, rep_table, rep_view, rep_scissor;']
base = 0
for tab, names, label in TABLES:
    out.append(f'H uint64_t {tab}_count[{len(names)}]; H void *{tab}_orig[{len(names)}];')
total = sum(len(t[1]) for t in TABLES)
out += [f'H uint64_t tm_ticks[{total}];',
        f'/* Timing: ioctl time by dxg ioctl number, and inside each method. */',
        'static uint64_t io_ticks[256], io_count[256], io_max[256], io_long[256];',
        '/* The latest ioctls over 2 ms: when (CLOCK_MONOTONIC ns at return), how long, which. */',
        'static uint64_t io_long_at[64]; static double io_long_ms[64]; static unsigned io_long_nr[64], io_long_n;',
        f'static uint64_t io_slot_ticks[{total}], io_slot_count[{total}];',
        '/* Timing: per thread, entries of (TSC at entry, return address, slot). */',
        '__attribute__((tls_model("initial-exec"), used, visibility("hidden"))) __thread uint64_t tsh_n;',
        '__attribute__((tls_model("initial-exec"), used, visibility("hidden"))) __thread uint64_t tsh_stack[256 * 3];',
        # Back from a timed call: add the time to the slot, return to the caller.
        '__asm__(".text\\n.p2align 4\\ntret:\\n sub $8, %rsp\\n push %rax\\n push %rdx\\n rdtsc\\n'
        ' shl $32, %rdx\\n or %rdx, %rax\\n movq tsh_n@gottpoff(%rip), %r11\\n subq $1, %fs:(%r11)\\n'
        ' movq %fs:(%r11), %r10\\n movq tsh_stack@gottpoff(%rip), %r11\\n lea (%r10,%r10,2), %r10\\n'
        ' lea (%r11,%r10,8), %r11\\n sub %fs:(%r11), %rax\\n movq %fs:16(%r11), %r10\\n'
        ' lea tm_ticks(%rip), %rdx\\n lock add %rax, (%rdx,%r10,8)\\n movq %fs:8(%r11), %rax\\n'
        ' movq %rax, 16(%rsp)\\n pop %rdx\\n pop %rax\\n ret\\n");', '']
for tab, names, label in TABLES:
    n = len(names)
    for k in range(n):
        out.append(f'__asm__(".text\\n.p2align 4\\n{tab}_thunk{k}:\\n lock incq {tab}_count+{8 * k}(%rip)\\n'
                   f' jmp *{tab}_orig+{8 * k}(%rip)\\n");')
        out.append(f'extern void {tab}_thunk{k}(void) __attribute__((visibility("hidden")));')
        # Timed: count, push (TSC, return address, slot), return through tret.
        # Only rax/rdx (saved) and r10/r11 (not argument registers) are used.
        out.append(f'__asm__(".text\\n.p2align 4\\n{tab}_tthunk{k}:\\n lock incq {tab}_count+{8 * k}(%rip)\\n'
                   ' push %rax\\n push %rdx\\n rdtsc\\n shl $32, %rdx\\n or %rdx, %rax\\n'
                   ' movq tsh_n@gottpoff(%rip), %r11\\n movq %fs:(%r11), %r10\\n cmpq $255, %r10\\n jae 1f\\n'
                   ' addq $1, %fs:(%r11)\\n movq tsh_stack@gottpoff(%rip), %r11\\n lea (%r10,%r10,2), %r10\\n'
                   ' lea (%r11,%r10,8), %r11\\n movq %rax, %fs:(%r11)\\n movq 16(%rsp), %rax\\n'
                   f' movq %rax, %fs:8(%r11)\\n movq ${base + k}, %fs:16(%r11)\\n lea tret(%rip), %rax\\n'
                   ' movq %rax, 16(%rsp)\\n1:\\n pop %rdx\\n pop %rax\\n'
                   f' jmp *{tab}_orig+{8 * k}(%rip)\\n");')
        out.append(f'extern void {tab}_tthunk{k}(void) __attribute__((visibility("hidden")));')
    out.append(f'static void *{tab}_thunks[{n}] = {{' + ', '.join(f'(void *){tab}_thunk{k}' for k in range(n)) + '};')
    out.append(f'static void *{tab}_tthunks[{n}] = {{' + ', '.join(f'(void *){tab}_tthunk{k}' for k in range(n)) + '};')
    out.append(f'static const char *{tab}_names[] = {{' + ', '.join(f'"{x}"' for x in names) + '};')
    base += n
out.append('struct table { const char *label; const char **names; uint64_t *count; int n, base; };')
out.append('static const struct table tables[] = {')
base = 0
for tab, names, label in TABLES:
    out.append(f'\t{{ "{label}", {tab}_names, {tab}_count, {len(names)}, {base} }},')
    base += len(names)
out.append('};')
out.append(f'#define N_TABLES {len(TABLES)}')
out.append(r'''
static void patch(void **vtable, void **orig, void **thunks, int n)
{
	long page = sysconf(_SC_PAGESIZE);
	uintptr_t a = (uintptr_t)vtable & ~(uintptr_t)(page - 1);
	uintptr_t e = ((uintptr_t)(vtable + n) + page - 1) & ~(uintptr_t)(page - 1);
	if (mprotect((void *)a, e - a, PROT_READ | PROT_WRITE)) {
		perror("d3d12count: mprotect");
		return;
	}
	for (int k = 0; k < n; k++) {
		if (vtable[k] == thunks[k])
			continue;
		orig[k] = vtable[k];
		vtable[k] = thunks[k];
	}
}

static int timing;
static double tsc_per_ns;

static void calibrate(void)
{
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	uint64_t t0 = __builtin_ia32_rdtsc();
	usleep(20000);
	clock_gettime(CLOCK_MONOTONIC, &b);
	uint64_t t1 = __builtin_ia32_rdtsc();
	tsc_per_ns = (double)(t1 - t0) / ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec));
}

struct row { const struct table *t; int k; };

static int by_time(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	uint64_t tx = tm_ticks[x->t->base + x->k], ty = tm_ticks[y->t->base + y->k];
	return tx < ty ? 1 : tx > ty ? -1 : 0;
}

static void dump(int sig)
{
	(void)sig;
	const char *path = getenv("D3D12COUNT_OUT");
	FILE *f = fopen(path ? path : "/tmp/d3d12count.txt", "w");
	if (!f)
		return;
	struct row rows[sizeof(tm_ticks) / sizeof(*tm_ticks)];
	int n = 0;
	for (int t = 0; t < N_TABLES; t++)
		for (int k = 0; k < tables[t].n; k++)
			if (tables[t].count[k])
				rows[n++] = (struct row){ &tables[t], k };
	if (timing) {
		qsort(rows, n, sizeof(*rows), by_time);
		double total = 0;
		for (int i = 0; i < n; i++)
			total += tm_ticks[rows[i].t->base + rows[i].k] / tsc_per_ns;
		fprintf(f, "%10s %10s %8s %9s %7s  (all calls %.2f ms; ioctl: time and count inside)\n",
			"calls", "ms", "ns/call", "ioctl ms", "ioctls", total / 1e6);
	}
	for (int i = 0; i < n; i++) {
		const struct table *t = rows[i].t;
		int k = rows[i].k;
		if (timing) {
			double ns = tm_ticks[t->base + k] / tsc_per_ns;
			fprintf(f, "%10lu %10.2f %8.0f %9.2f %7lu  %-7s %s\n", (unsigned long)t->count[k], ns / 1e6,
				ns / t->count[k], io_slot_ticks[t->base + k] / tsc_per_ns / 1e6,
				(unsigned long)io_slot_count[t->base + k], t->label, t->names[k]);
		} else {
			fprintf(f, "%10lu %-7s %s\n", (unsigned long)t->count[k], t->label, t->names[k]);
		}
	}
	if (!timing) {
		fprintf(f, "%10lu repeat  SetPipelineState_same_PSO\n", (unsigned long)rep_pso);
		fprintf(f, "%10lu repeat  SetGraphicsRootSignature_same\n", (unsigned long)rep_rs);
		fprintf(f, "%10lu repeat  IASetVertexBuffers_same_view\n", (unsigned long)rep_vb);
		fprintf(f, "%10lu repeat  SetGraphicsRootDescriptorTable_same_index_handle\n", (unsigned long)rep_table);
		fprintf(f, "%10lu repeat  RSSetViewports_same\n", (unsigned long)rep_view);
		fprintf(f, "%10lu repeat  RSSetScissorRects_same\n", (unsigned long)rep_scissor);
	}
	if (timing) {
		fprintf(f, "\n%10s %10s %8s %8s %6s  dxg ioctl (all threads)\n", "calls", "ms", "us/call",
			"max ms", ">2ms");
		for (int k = 0; k < 256; k++)
			if (io_count[k])
				fprintf(f, "%10lu %10.2f %8.1f %8.2f %6lu  0x%02x\n", (unsigned long)io_count[k],
					io_ticks[k] / tsc_per_ns / 1e6, io_ticks[k] / tsc_per_ns / 1e3 / io_count[k],
					io_max[k] / tsc_per_ns / 1e6, (unsigned long)io_long[k], k);
		fprintf(f, "\nioctls over 2 ms (returned at, UTC as WAYLAND_DEBUG; ms; nr):\n");
		unsigned n = io_long_n < 64 ? io_long_n : 64;
		for (unsigned i = 0; i < n; i++) {
			uint64_t s = io_long_at[i] / 1000000000ull % 86400;
			fprintf(f, "  [%02u:%02u:%09.6f] %8.2f 0x%02x\n", (unsigned)(s / 3600), (unsigned)(s / 60 % 60),
				s % 60 + (io_long_at[i] % 1000000000ull) / 1e9, io_long_ms[i], io_long_nr[i]);
		}
		io_long_n = 0;
		memset(io_max, 0, sizeof(io_max));
		memset(io_long, 0, sizeof(io_long));
	}
	rep_pso = rep_rs = rep_vb = rep_table = rep_view = rep_scissor = 0;
	fclose(f);
	memset(tm_ticks, 0, sizeof(tm_ticks));
	memset(io_ticks, 0, sizeof(io_ticks));
	memset(io_count, 0, sizeof(io_count));
	memset(io_slot_ticks, 0, sizeof(io_slot_ticks));
	memset(io_slot_count, 0, sizeof(io_slot_count));
	for (int t = 0; t < N_TABLES; t++)
		memset(tables[t].count, 0, tables[t].n * sizeof(uint64_t));
}

/* Repeats: calls whose arguments equal the previous call's on that list. */
static void *last_pso, *last_rs;
static uint64_t last_vb[3], last_table[64];
static uint32_t last_view[6], last_scissor[4];
typedef void (*set_ptr_fn)(void *, void *);
typedef void (*set_vb_fn)(void *, unsigned, unsigned, const uint64_t *);
typedef void (*set_table_fn)(void *, unsigned, uint64_t);
typedef void (*set_rects_fn)(void *, unsigned, const uint32_t *);
static void w_set_pso(void *self, void *pso)
{
	cl_count[25]++;
	if (pso == last_pso) rep_pso++;
	last_pso = pso;
	((set_ptr_fn)cl_orig[25])(self, pso);
}
static void w_set_rs(void *self, void *rs)
{
	cl_count[30]++;
	if (rs == last_rs) rep_rs++;
	last_rs = rs;
	((set_ptr_fn)cl_orig[30])(self, rs);
}
static void w_set_vb(void *self, unsigned start, unsigned n, const uint64_t *views)
{
	cl_count[44]++;
	if (start == 0 && n == 1 && views && !memcmp(views, last_vb, 16)) rep_vb++;
	if (start == 0 && n >= 1 && views) memcpy(last_vb, views, 16);
	((set_vb_fn)cl_orig[44])(self, start, n, views);
}
static void w_set_table(void *self, unsigned index, uint64_t handle)
{
	cl_count[32]++;
	if (index < 64) {
		if (last_table[index] == handle) rep_table++;
		last_table[index] = handle;
	}
	((set_table_fn)cl_orig[32])(self, index, handle);
}
static void w_set_views(void *self, unsigned n, const uint32_t *v)
{
	cl_count[21]++;
	if (n == 1 && v && !memcmp(v, last_view, 24)) rep_view++;
	if (n >= 1 && v) memcpy(last_view, v, 24);
	((set_rects_fn)cl_orig[21])(self, n, v);
}
static void w_set_scissor(void *self, unsigned n, const uint32_t *v)
{
	cl_count[22]++;
	if (n == 1 && v && !memcmp(v, last_scissor, 16)) rep_scissor++;
	if (n >= 1 && v) memcpy(last_scissor, v, 16);
	((set_rects_fn)cl_orig[22])(self, n, v);
}
static void wrap_cmdlist(void **vt)
{
	/* After patch(): the originals are saved; swap in the argument-aware
	 * wrappers. */
	vt[25] = (void *)w_set_pso;
	vt[30] = (void *)w_set_rs;
	vt[44] = (void *)w_set_vb;
	vt[32] = (void *)w_set_table;
	vt[21] = (void *)w_set_views;
	vt[22] = (void *)w_set_scissor;
}

/* IIDs: ID3D12CommandAllocator, ID3D12GraphicsCommandList, ID3D12CommandQueue,
 * ID3D12Fence, ID3D12DeviceFactory. */
static const unsigned char iid_alloc[16] = { 0xe4,0xde,0x02,0x61,0x59,0xaf,0x09,0x4b,0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24 };
static const unsigned char iid_gcl[16] = { 0x0f,0x0d,0x16,0x5b,0x1b,0xac,0x85,0x41,0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55 };
static const unsigned char iid_queue[16] = { 0xa6,0x70,0xc8,0x0e,0x7e,0x5d,0x22,0x4c,0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed };
static const unsigned char iid_fence[16] = { 0xcf,0x3d,0x75,0x0a,0xd8,0xc4,0x91,0x4b,0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76 };
static const unsigned char iid_factory[16] = { 0xd3,0x07,0xf3,0x61,0x4e,0xd3,0x7c,0x4e,0x83,0x74,0x3b,0xa4,0xde,0x23,0xcc,0xcb };

static int installed;

/* Redirects the vtables of the device and of a command list, queue and
 * fence made on it for the purpose (every object of a class shares one). */
static void install(void *dev)
{
	if (installed)
		return;
	installed = 1;
	timing = getenv("D3D12COUNT_TIME") != NULL;
	if (timing)
		calibrate();
	void **dvt = *(void ***)dev;
	typedef long (*mk_alloc)(void *, int, const void *, void **);
	typedef long (*mk_list)(void *, unsigned, int, void *, void *, const void *, void **);
	typedef long (*mk_queue)(void *, const void *, const void *, void **);
	typedef long (*mk_fence)(void *, uint64_t, int, const void *, void **);
	typedef unsigned long (*rel)(void *);
	void *alloc = NULL, *list = NULL, *queue = NULL, *fence = NULL;
	if (((mk_alloc)dvt[9])(dev, 0, iid_alloc, &alloc) >= 0 &&
	    ((mk_list)dvt[12])(dev, 0, 0, alloc, NULL, iid_gcl, &list) >= 0) {
		patch(*(void ***)list, cl_orig, timing ? cl_tthunks : cl_thunks, sizeof(cl_thunks) / sizeof(*cl_thunks));
		if (!timing)
			wrap_cmdlist(*(void ***)list);
		((rel)(*(void ***)list)[2])(list);
	}
	if (alloc)
		((rel)(*(void ***)alloc)[2])(alloc);
	uint32_t qdesc[4] = { 0, 0, 0, 0 };   /* direct, normal priority */
	if (((mk_queue)dvt[8])(dev, qdesc, iid_queue, &queue) >= 0) {
		patch(*(void ***)queue, q_orig, timing ? q_tthunks : q_thunks, sizeof(q_thunks) / sizeof(*q_thunks));
		((rel)(*(void ***)queue)[2])(queue);
	}
	if (((mk_fence)dvt[36])(dev, 0, 0, iid_fence, &fence) >= 0) {
		patch(*(void ***)fence, fn_orig, timing ? fn_tthunks : fn_thunks, sizeof(fn_thunks) / sizeof(*fn_thunks));
		((rel)(*(void ***)fence)[2])(fence);
	}
	patch(dvt, dev_orig, timing ? dev_tthunks : dev_thunks, sizeof(dev_thunks) / sizeof(*dev_thunks));
	signal(SIGUSR1, dump);
	fprintf(stderr, "d3d12count: %s D3D12 calls (SIGUSR1 writes them)\n", timing ? "timing" : "counting");
}

typedef long (*create_fn)(void *, int, const void *, void **);
static create_fn real_create;

static long hooked_create(void *adapter, int level, const void *riid, void **out)
{
	long hr = real_create(adapter, level, riid, out);
	if (hr >= 0 && out && *out)
		install(*out);
	return hr;
}

/* Mesa creates its device through D3D12GetInterface(CLSID_D3D12DeviceFactory)
 * and ID3D12DeviceFactory::CreateDevice (vtable slot 9). */
typedef long (*getif_fn)(const void *, const void *, void **);
typedef long (*factory_create_fn)(void *, void *, int, const void *, void **);
static getif_fn real_getif;
static factory_create_fn real_factory_create;

static long hooked_factory_create(void *self, void *adapter, int level, const void *riid, void **out)
{
	long hr = real_factory_create(self, adapter, level, riid, out);
	if (hr >= 0 && out && *out)
		install(*out);
	return hr;
}

static long hooked_getif(const void *clsid, const void *riid, void **out)
{
	long hr = real_getif(clsid, riid, out);
	if (hr >= 0 && out && *out && riid && !memcmp(riid, iid_factory, 16)) {
		void **fvt = *(void ***)*out;
		if (fvt[9] != (void *)hooked_factory_create) {
			real_factory_create = (factory_create_fn)fvt[9];
			long page = sysconf(_SC_PAGESIZE);
			uintptr_t a = (uintptr_t)&fvt[9] & ~(uintptr_t)(page - 1);
			if (!mprotect((void *)a, page * 2, PROT_READ | PROT_WRITE))
				fvt[9] = (void *)hooked_factory_create;
		}
	}
	return hr;
}

/* Timing mode: the dxg driver's ioctls (libdxcore calls ioctl through the
 * PLT), by number and by the innermost timed D3D12 method around them. */
typedef int (*ioctl_fn)(int, unsigned long, void *);
int ioctl(int fd, unsigned long req, ...)
{
	static ioctl_fn real;
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (!real)
		real = (ioctl_fn)dlvsym(RTLD_NEXT, "ioctl", "GLIBC_2.2.5");
	if (!timing)
		return real(fd, req, arg);
	uint64_t t0 = __builtin_ia32_rdtsc();
	int r = real(fd, req, arg);
	uint64_t dt = __builtin_ia32_rdtsc() - t0;
	unsigned nr = ((req >> 8) & 0xff) == 0x47 ? (req & 0xff) : 255;
	__atomic_add_fetch(&io_ticks[nr], dt, __ATOMIC_RELAXED);
	__atomic_add_fetch(&io_count[nr], 1, __ATOMIC_RELAXED);
	if (tsh_n > 0 && tsh_n <= 255) {
		uint64_t slot = tsh_stack[(tsh_n - 1) * 3 + 2];
		__atomic_add_fetch(&io_slot_ticks[slot], dt, __ATOMIC_RELAXED);
		__atomic_add_fetch(&io_slot_count[slot], 1, __ATOMIC_RELAXED);
	}
	uint64_t old = __atomic_load_n(&io_max[nr], __ATOMIC_RELAXED);
	while (dt > old && !__atomic_compare_exchange_n(&io_max[nr], &old, dt, 0, __ATOMIC_RELAXED,
							 __ATOMIC_RELAXED))
		;
	double ms = dt / tsc_per_ns / 1e6;
	if (ms > 2.0) {
		__atomic_add_fetch(&io_long[nr], 1, __ATOMIC_RELAXED);
		unsigned i = __atomic_fetch_add(&io_long_n, 1, __ATOMIC_RELAXED) % 64;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		io_long_at[i] = ts.tv_sec * 1000000000ull + ts.tv_nsec;
		io_long_ms[i] = ms;
		io_long_nr[i] = nr;
	}
	return r;
}

typedef void *(*dlsym_fn)(void *, const char *);
void *dlsym(void *handle, const char *name)
{
	static dlsym_fn real;
	if (!real)
		real = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
	if (!real)
		real = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
	void *p = real(handle, name);
	if (name && getenv("D3D12COUNT_DEBUG") && strstr(name, "D3D12"))
		fprintf(stderr, "d3d12count: dlsym %s -> %p\n", name, p);
	if (p && name && !strcmp(name, "D3D12CreateDevice")) {
		real_create = (create_fn)p;
		return (void *)hooked_create;
	}
	if (p && name && !strcmp(name, "D3D12GetInterface")) {
		real_getif = (getif_fn)p;
		return (void *)hooked_getif;
	}
	return p;
}
''')
open(sys.argv[1], 'w', newline='\n').write('\n'.join(out) + '\n')
print('written', sys.argv[1], f'({len(CL)} cmdlist, {len(DEV)} device, {len(QUEUE)} queue, {len(FENCE)} fence slots)')
