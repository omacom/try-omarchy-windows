// SPDX-License-Identifier: MIT
/*
 * DXCore adapter enumeration and D3D12 device creation through /dev/dxg,
 * with Microsoft's libdxcore/libd3d12 (as Mesa's d3d12 and Dozen drivers
 * use them). Plain C through the COM vtables, so no SDK is needed.
 *
 *   gcc -O2 -o d3d12test d3d12test.c -ldl
 *   LD_LIBRARY_PATH=/usr/lib/wsl/lib ./d3d12test
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t HRESULT;
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } GUID;
typedef void *(*vfn)(void);
typedef struct { vfn *vtbl; } Obj;

static const GUID IID_IDXCoreAdapterFactory = { 0x78ee5945, 0xc36e, 0x4b13,
	{ 0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06 } };
static const GUID IID_IDXCoreAdapterList = { 0x526c7776, 0x40e9, 0x459b,
	{ 0xb7, 0x11, 0xf3, 0x2a, 0xd7, 0x6d, 0xfc, 0x28 } };
static const GUID IID_IDXCoreAdapter = { 0xf0db4c7f, 0xfe5a, 0x42a2,
	{ 0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e } };
static const GUID D3D12_GRAPHICS = { 0x0c9ece4d, 0x2f6e, 0x4f01,
	{ 0x8c, 0x96, 0xe8, 0x9e, 0x33, 0x1b, 0x47, 0xb1 } };
static const GUID D3D12_CORE_COMPUTE = { 0x248e2800, 0xa793, 0x4724,
	{ 0xab, 0xaa, 0x23, 0xa6, 0xde, 0x1b, 0xe0, 0x90 } };
static const GUID IID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57,
	{ 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };

#define CALL(obj, idx, type, ...) \
	((type)((Obj *)(obj))->vtbl[idx])(obj, ##__VA_ARGS__)

/* ID3D12Device::CheckFeatureSupport (slot 13). */
static HRESULT feature(void *device, int which, void *data, uint32_t size)
{
	memset(data, 0, size);
	return CALL(device, 13, HRESULT (*)(void *, int, void *, uint32_t),
		    which, data, size);
}

static const char *tier(uint32_t t)
{
	static char buf[32];

	if (!t)
		return "no";
	snprintf(buf, sizeof(buf), "tier %u.%u", t / 10, t % 10);
	return buf;
}

/* The DirectX 12 Ultimate features, as the guest's applications see them. */
static void features(void *device)
{
	uint32_t d[16];
	int sm;

	/* D3D12_FEATURE_SHADER_MODEL (7): in, the highest model asked. */
	for (sm = 0x69; sm >= 0x60; sm--) {
		d[0] = sm;
		if (CALL(device, 13, HRESULT (*)(void *, int, void *, uint32_t),
			 7, d, 4) >= 0)
			break;
	}
	if (sm >= 0x60)
		printf("shader model: %x.%x\n", d[0] >> 4, d[0] & 15);
	/* OPTIONS5 (27): SRVOnlyTiledResourceTier3, RenderPassesTier,
	 * RaytracingTier. */
	if (feature(device, 27, d, 12) >= 0)
		printf("ray tracing (DXR): %s\n", tier(d[2]));
	/* OPTIONS6 (30): ..., VariableShadingRateTier at 8. */
	if (feature(device, 30, d, 20) >= 0)
		printf("variable rate shading: tier %u\n", d[2]);
	/* OPTIONS7 (32): MeshShaderTier, SamplerFeedbackTier. */
	if (feature(device, 32, d, 8) >= 0) {
		printf("mesh shaders: %s\n", tier(d[0]));
		printf("sampler feedback: %s\n", tier(d[1] / 10));
	}
	/* OPTIONS1 (8): WaveOps, WaveLaneCountMin, Max, TotalLaneCount. */
	if (feature(device, 8, d, 24) >= 0)
		printf("wave ops: %s, %u lanes, %u in total\n",
		       d[0] ? "yes" : "no", d[1], d[3]);
	/* OPTIONS (0): TiledResourcesTier at 12, ResourceBindingTier at 16,
	 * ConservativeRasterizationTier at 32. */
	if (feature(device, 0, d, 60) >= 0)
		printf("tiled resources: tier %u, resource binding: tier %u, "
		       "conservative rasterization: tier %u\n",
		       d[3], d[4], d[8]);
}

int main(void)
{
	void *dxcore = dlopen("libdxcore.so", RTLD_NOW);
	void *d3d12 = dlopen("libd3d12.so", RTLD_NOW);
	HRESULT (*create_factory)(const GUID *, void **);
	HRESULT (*create_device)(void *, int, const GUID *, void **);
	void *factory = NULL, *list = NULL, *device = NULL;
	uint32_t i, count;
	HRESULT hr;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!dxcore || !d3d12) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	create_factory = dlsym(dxcore, "DXCoreCreateAdapterFactory");
	create_device = dlsym(d3d12, "D3D12CreateDevice");
	hr = create_factory(&IID_IDXCoreAdapterFactory, &factory);
	printf("DXCoreCreateAdapterFactory: 0x%08x\n", (unsigned)hr);
	if (hr < 0)
		return 1;
	for (i = 0; i < 2; i++) {
		const GUID *attr = i ? &D3D12_CORE_COMPUTE : &D3D12_GRAPHICS;

		list = NULL;
		/* IDXCoreAdapterFactory::CreateAdapterList (slot 3) */
		hr = CALL(factory, 3, HRESULT (*)(void *, uint32_t, const GUID *,
			  const GUID *, void **), 1, attr, &IID_IDXCoreAdapterList,
			  &list);
		count = hr >= 0 ? CALL(list, 4, uint32_t (*)(void *)) : 0;
		printf("%s adapters: 0x%08x, %u\n", i ? "core compute" : "graphics",
		       (unsigned)hr, count);
		if (!i && count) {
			void *adapter = NULL;
			char name[256] = "";

			/* IDXCoreAdapterList::GetAdapter (3);
			 * IDXCoreAdapter::GetProperty (6), DriverDescription = 2 */
			CALL(list, 3, HRESULT (*)(void *, uint32_t, const GUID *,
			     void **), 0, &IID_IDXCoreAdapter, &adapter);
			CALL(adapter, 6, HRESULT (*)(void *, int, size_t, void *),
			     2, sizeof(name), name);
			printf("adapter 0: %s\n", name);
			/* 12_2 is DirectX 12 Ultimate; 11_0 is what Mesa asks. */
			hr = create_device(adapter, 0xc200, &IID_ID3D12Device,
					   &device);
			printf("D3D12CreateDevice(feature level 12_2): 0x%08x\n",
			       (unsigned)hr);
			if (hr < 0) {
				hr = create_device(adapter, 0xb000,
						   &IID_ID3D12Device, &device);
				printf("D3D12CreateDevice(feature level 11_0): 0x%08x\n",
				       (unsigned)hr);
			}
			if (hr >= 0) {
				features(device);
				printf("D3D12 TEST PASSED\n");
			}
		}
	}
	return device ? 0 : 1;
}
