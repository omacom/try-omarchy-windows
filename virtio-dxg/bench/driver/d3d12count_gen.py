"""d3d12count_gen.py OUT.c: writes d3d12count.c, an LD_PRELOAD library that
counts every ID3D12Device and ID3D12GraphicsCommandList method call (by
vtable slot) of a process: Mesa looks up D3D12CreateDevice with dlsym, which
the library wraps; the device's and a command list's vtables are redirected
through counting thunks. SIGUSR1 writes the counts since the last signal to
$D3D12COUNT_OUT (default /tmp/d3d12count.txt) and resets them.
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
       'CreateCommandSignature GetResourceTiling GetAdapterLuid').split()
N_CL, N_DEV = 96, 96

out = ['#define _GNU_SOURCE',
       '#include <dlfcn.h>', '#include <signal.h>', '#include <stdint.h>', '#include <stdio.h>',
       '#include <stdlib.h>', '#include <string.h>', '#include <sys/mman.h>', '#include <unistd.h>', '',
       '#define H __attribute__((visibility("hidden")))',
       f'H uint64_t cl_count[{N_CL}]; H void *cl_orig[{N_CL}];',
       f'H uint64_t dev_count[{N_DEV}]; H void *dev_orig[{N_DEV}];',
       'static uint64_t rep_pso, rep_rs, rep_vb, rep_table, rep_view, rep_scissor;', '']
for tab, n in (('cl', N_CL), ('dev', N_DEV)):
    for k in range(n):
        out.append(f'__asm__(".text\\n.p2align 4\\n{tab}_thunk{k}:\\n lock incq {tab}_count+{8 * k}(%rip)\\n'
                   f' jmp *{tab}_orig+{8 * k}(%rip)\\n");')
        out.append(f'extern void {tab}_thunk{k}(void) __attribute__((visibility("hidden")));')
    out.append(f'static void *{tab}_thunks[{n}] = {{' + ', '.join(f'(void *){tab}_thunk{k}' for k in range(n)) + '};')
names = lambda tab, lst: f'static const char *{tab}_names[] = {{' + ', '.join(f'"{x}"' for x in lst) + '};'
out.append(names('cl', CL))
out.append(names('dev', DEV))
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

static void dump(int sig)
{
	(void)sig;
	const char *path = getenv("D3D12COUNT_OUT");
	FILE *f = fopen(path ? path : "/tmp/d3d12count.txt", "w");
	if (!f)
		return;
	for (int k = 0; k < (int)(sizeof(cl_names) / sizeof(*cl_names)); k++)
		if (cl_count[k])
			fprintf(f, "%10lu cmdlist %s\n", (unsigned long)cl_count[k], cl_names[k]);
	for (int k = 0; k < (int)(sizeof(dev_names) / sizeof(*dev_names)); k++)
		if (dev_count[k])
			fprintf(f, "%10lu device  %s\n", (unsigned long)dev_count[k], dev_names[k]);
	fprintf(f, "%10lu repeat  SetPipelineState_same_PSO\n", (unsigned long)rep_pso);
	fprintf(f, "%10lu repeat  SetGraphicsRootSignature_same\n", (unsigned long)rep_rs);
	fprintf(f, "%10lu repeat  IASetVertexBuffers_same_view\n", (unsigned long)rep_vb);
	fprintf(f, "%10lu repeat  SetGraphicsRootDescriptorTable_same_index_handle\n", (unsigned long)rep_table);
	fprintf(f, "%10lu repeat  RSSetViewports_same\n", (unsigned long)rep_view);
	fprintf(f, "%10lu repeat  RSSetScissorRects_same\n", (unsigned long)rep_scissor);
	rep_pso = rep_rs = rep_vb = rep_table = rep_view = rep_scissor = 0;
	fclose(f);
	memset(cl_count, 0, sizeof(cl_count));
	memset(dev_count, 0, sizeof(dev_count));
}

typedef long (*create_fn)(void *, int, const void *, void **);
static create_fn real_create;
static int installed;

/* IIDs: ID3D12CommandAllocator, ID3D12GraphicsCommandList. */
static const unsigned char iid_alloc[16] = { 0xe4,0xde,0x02,0x61,0x59,0xaf,0x09,0x4b,0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24 };
static const unsigned char iid_gcl[16] = { 0x0f,0x0d,0x16,0x5b,0x1b,0xac,0x85,0x41,0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55 };

static long hooked_create(void *adapter, int level, const void *riid, void **out)
{
	long hr = real_create(adapter, level, riid, out);
	if (hr < 0 || !out || !*out || installed)
		return hr;
	installed = 1;
	void *dev = *out;
	void **dvt = *(void ***)dev;
	typedef long (*mk_alloc)(void *, int, const void *, void **);
	typedef long (*mk_list)(void *, unsigned, int, void *, void *, const void *, void **);
	void *alloc = NULL, *list = NULL;
	if (((mk_alloc)dvt[9])(dev, 0, iid_alloc, &alloc) >= 0 &&
	    ((mk_list)dvt[12])(dev, 0, 0, alloc, NULL, iid_gcl, &list) >= 0) {
		patch(*(void ***)list, cl_orig, cl_thunks, sizeof(cl_thunks) / sizeof(*cl_thunks));
		typedef unsigned long (*rel)(void *);
		((rel)(*(void ***)list)[2])(list);
	}
	if (alloc) {
		typedef unsigned long (*rel)(void *);
		((rel)(*(void ***)alloc)[2])(alloc);
	}
	patch(dvt, dev_orig, dev_thunks, sizeof(dev_thunks) / sizeof(*dev_thunks));
	signal(SIGUSR1, dump);
	fprintf(stderr, "d3d12count: counting D3D12 calls (SIGUSR1 writes them)\n");
	return hr;
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
	 * wrappers. A new root signature resets the bound tables. */
	vt[25] = (void *)w_set_pso;
	vt[30] = (void *)w_set_rs;
	vt[44] = (void *)w_set_vb;
	vt[32] = (void *)w_set_table;
	vt[21] = (void *)w_set_views;
	vt[22] = (void *)w_set_scissor;
}

/* Mesa creates its device through D3D12GetInterface(CLSID_D3D12DeviceFactory)
 * and ID3D12DeviceFactory::CreateDevice (vtable slot 9). */
typedef long (*getif_fn)(const void *, const void *, void **);
typedef long (*factory_create_fn)(void *, void *, int, const void *, void **);
static getif_fn real_getif;
static factory_create_fn real_factory_create;
static const unsigned char iid_factory[16] = { 0xd3,0x07,0xf3,0x61, 0x4e,0xd3, 0x7c,0x4e, 0x83,0x74,0x3b,0xa4,0xde,0x23,0xcc,0xcb };

static long hooked_factory_create(void *self, void *adapter, int level, const void *riid, void **out)
{
	real_create = NULL;
	long hr = real_factory_create(self, adapter, level, riid, out);
	if (hr >= 0 && out && *out && !installed) {
		/* Reuse the device hook body with a pass-through creator. */
		real_create = NULL;
		void *dev = *out;
		installed = 1;
		void **dvt = *(void ***)dev;
		typedef long (*mk_alloc)(void *, int, const void *, void **);
		typedef long (*mk_list)(void *, unsigned, int, void *, void *, const void *, void **);
		typedef unsigned long (*rel)(void *);
		void *alloc = NULL, *list = NULL;
		if (((mk_alloc)dvt[9])(dev, 0, iid_alloc, &alloc) >= 0 &&
		    ((mk_list)dvt[12])(dev, 0, 0, alloc, NULL, iid_gcl, &list) >= 0) {
			patch(*(void ***)list, cl_orig, cl_thunks, sizeof(cl_thunks) / sizeof(*cl_thunks));
			wrap_cmdlist(*(void ***)list);
			((rel)(*(void ***)list)[2])(list);
		}
		if (alloc)
			((rel)(*(void ***)alloc)[2])(alloc);
		patch(dvt, dev_orig, dev_thunks, sizeof(dev_thunks) / sizeof(*dev_thunks));
		signal(SIGUSR1, dump);
		fprintf(stderr, "d3d12count: counting D3D12 calls (SIGUSR1 writes them)\n");
	}
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

__attribute__((constructor)) static void loaded(void)
{
	if (getenv("D3D12COUNT_DEBUG"))
		fprintf(stderr, "d3d12count: loaded\n");
}
''')
open(sys.argv[1], 'w', newline='\n').write('\n'.join(out) + '\n')
print('written', sys.argv[1])
