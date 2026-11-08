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
			hr = create_device(adapter, 0xb000, &IID_ID3D12Device,
					   &device);
			printf("D3D12CreateDevice(feature level 11_0): 0x%08x%s\n",
			       (unsigned)hr, hr >= 0 ? "  D3D12 TEST PASSED" : "");
		}
	}
	return device ? 0 : 1;
}
