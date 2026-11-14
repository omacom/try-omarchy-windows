// SPDX-License-Identifier: MIT
/*
 * dxgs.dll: the DXGI the Khronos Vulkan loader sees under Wine/Proton.
 *
 * The Windows loader loads system32\dxgi.dll and creates a DXGI factory
 * while it reads the registered drivers (it checks Intel/NVIDIA/AMD's own
 * manifests against the adapters' vendors) and while it sorts physical
 * devices. Under Proton dxgi.dll is DXVK's, whose factory creates a Vulkan
 * instance through this same loader: the loader re-enters itself and
 * deadlocks. patch_loader.py points the loader here instead.
 *
 * The factory has no adapters: registered drivers that are not one of
 * those vendors' manifests (dxgvk.json) are accepted as they are, and the
 * physical-device order falls back to the drivers' own (IDXGIFactory6 is
 * not offered). It creates nothing and needs no GPU.
 */
#include <windows.h>

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887a0002)
#endif

static const GUID iid_unknown = { 0x00000000, 0x0000, 0x0000,
	{ 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID iid_object = { 0xaec22fb8, 0x76f3, 0x4639,
	{ 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e } };
static const GUID iid_factory = { 0x7b7166ec, 0x21c7, 0x44ae,
	{ 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba,
	{ 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };

struct factory {
	void *const *vtbl;
	volatile LONG refs;
};

static BOOL known(REFIID riid)
{
	return IsEqualGUID(riid, &iid_unknown) || IsEqualGUID(riid, &iid_object) ||
	       IsEqualGUID(riid, &iid_factory) || IsEqualGUID(riid, &iid_factory1);
}

static HRESULT WINAPI query(struct factory *f, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!known(riid)) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&f->refs);
	*out = f;
	return S_OK;
}

static ULONG WINAPI add_ref(struct factory *f)
{
	return (ULONG)InterlockedIncrement(&f->refs);
}

static ULONG WINAPI release(struct factory *f)
{
	LONG refs = InterlockedDecrement(&f->refs);

	if (!refs)
		HeapFree(GetProcessHeap(), 0, f);
	return (ULONG)refs;
}

static HRESULT WINAPI not_implemented(void)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI enum_adapters(struct factory *f, UINT index, void **adapter)
{
	(void)f; (void)index;
	if (adapter)
		*adapter = NULL;
	return DXGI_ERROR_NOT_FOUND;
}

static BOOL WINAPI is_current(struct factory *f)
{
	(void)f;
	return TRUE;
}

/* IUnknown, IDXGIObject (4), IDXGIFactory (5), IDXGIFactory1 (2) */
static void *const factory_vtbl[] = {
	(void *)query, (void *)add_ref, (void *)release,
	(void *)not_implemented, (void *)not_implemented,
	(void *)not_implemented, (void *)not_implemented,
	(void *)enum_adapters, (void *)not_implemented, (void *)not_implemented,
	(void *)not_implemented, (void *)not_implemented,
	(void *)enum_adapters, (void *)is_current,
};

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out)
{
	struct factory *f;

	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!known(riid))
		return E_NOINTERFACE;
	f = HeapAlloc(GetProcessHeap(), 0, sizeof(*f));
	if (!f)
		return E_OUTOFMEMORY;
	f->vtbl = factory_vtbl;
	f->refs = 1;
	*out = f;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **out)
{
	return CreateDXGIFactory1(riid, out);
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **out)
{
	(void)flags;
	return CreateDXGIFactory1(riid, out);
}
