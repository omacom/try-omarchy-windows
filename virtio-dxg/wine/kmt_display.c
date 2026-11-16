// SPDX-License-Identifier: MIT
/*
 * GDI's view of the GPUs, built from the dxg adapters.
 *
 * NVIDIA's user-mode driver finds its adapter the way it does on Windows:
 * EnumDisplayDevicesA, a display device whose DeviceID contains VEN_10DE,
 * CreateDCA on it, then D3DKMTOpenAdapterFromHdc. Wine describes the GPUs
 * of the Linux display stack (in the VM, virtio-gpu), which the driver
 * ignores, so it would find no adapter.
 *
 * Every display Wine reports is shown here as belonging to the primary dxg
 * GPU, with that GPU's PCI IDs and name read from the kernel, and a DC or
 * display name opens that GPU. This is the Hyper-V GPU-P arrangement: the
 * paravirtualized GPU renders everything, and frames reach the screen
 * through the VM's display device. The primary GPU is the first NVIDIA
 * adapter, or the first adapter when there is none.
 *
 * The product version does the same inside Wine's win32u GPU list.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "kmt_bridge.h"

#define MAX_GPUS 8
#define QAI_PHYSICALADAPTERDEVICEIDS 31
#define QAI_DRIVER_DESCRIPTION_RENDER 66
#define QAI_DRIVER_DESCRIPTION 65

struct device_ids {
	uint32_t physical_adapter_index;
	uint32_t vendor;
	uint32_t device;
	uint32_t subsystem_vendor;
	uint32_t subsystem;
	uint32_t revision;
	uint32_t bus_type;
};

struct gpu {
	struct winluid luid;
	struct device_ids ids;
	wchar_t name[128];
};

static struct gpu gpus[MAX_GPUS];
static int gpu_count;
static INIT_ONCE scanned = INIT_ONCE_STATIC_INIT;

typedef int32_t(WINAPI *kmt_call)(void *);

static int32_t kmt(const char *name, void *args)
{
	kmt_call f = (kmt_call)kmt_bridge_lookup(name);

	return f ? f(args) : (int32_t)0xC00000BB;
}

static int32_t query(struct d3dkmthandle adapter, uint32_t type, void *data,
		     uint32_t size)
{
	struct d3dkmt_queryadapterinfo q = { 0 };

	q.adapter = adapter;
	q.type = type;
	q.private_data = (uint64_t)(uintptr_t)data;
	q.private_data_size = size;
	return kmt("D3DKMTQueryAdapterInfo", &q);
}

static void describe(struct gpu *g, struct d3dkmthandle adapter)
{
	static wchar_t text[4096];

	if (query(adapter, QAI_PHYSICALADAPTERDEVICEIDS, &g->ids,
		  sizeof(g->ids)) < 0)
		memset(&g->ids, 0, sizeof(g->ids));
	if (query(adapter, QAI_DRIVER_DESCRIPTION_RENDER, text,
		  sizeof(text)) >= 0 ||
	    query(adapter, QAI_DRIVER_DESCRIPTION, text, sizeof(text)) >= 0)
		wcsncpy(g->name, text, 127);
	else
		swprintf(g->name, 128, L"GPU %04X:%04X", g->ids.vendor,
			 g->ids.device);
}

static BOOL CALLBACK scan(INIT_ONCE *once, void *param, void **context)
{
	struct d3dkmt_adapterinfo info[MAX_GPUS];
	struct d3dkmt_enumadapters2 e = { MAX_GPUS, 0,
					  (void *)info };
	uint32_t i;
	int primary = -1;

	(void)once, (void)param, (void)context;
	if (kmt("D3DKMTEnumAdapters2", &e) < 0)
		return TRUE;
	for (i = 0; i < e.num_adapters && i < MAX_GPUS; i++) {
		struct d3dkmt_closeadapter c = { info[i].adapter_handle };
		struct gpu *g = &gpus[gpu_count++];

		g->luid = info[i].adapter_luid;
		describe(g, info[i].adapter_handle);
		kmt("D3DKMTCloseAdapter", &c);
		if (primary < 0 && g->ids.vendor == 0x10de)
			primary = gpu_count - 1;
	}
	if (primary > 0) {
		struct gpu t = gpus[0];

		gpus[0] = gpus[primary];
		gpus[primary] = t;
	}
	for (i = 0; i < (uint32_t)gpu_count; i++)
		fprintf(stderr, "[kmt] gpu %u: %ls, PCI %04x:%04x subsys "
			"%04x%04x rev %02x, luid %08x:%08x%s\n", i,
			gpus[i].name, gpus[i].ids.vendor, gpus[i].ids.device,
			gpus[i].ids.subsystem, gpus[i].ids.subsystem_vendor,
			gpus[i].ids.revision, gpus[i].luid.b, gpus[i].luid.a,
			i ? "" : " (primary)");
	return TRUE;
}

static const struct gpu *primary_gpu(void)
{
	InitOnceExecuteOnce(&scanned, scan, NULL, NULL);
	return gpu_count ? &gpus[0] : NULL;
}

static void pci_id(const struct gpu *g, char *out, size_t size)
{
	snprintf(out, size, "PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
		 g->ids.vendor, g->ids.device, g->ids.subsystem,
		 g->ids.subsystem_vendor, g->ids.revision);
}

static BOOL WINAPI display_EnumDisplayDevicesA(LPCSTR device, DWORD index,
					       DISPLAY_DEVICEA *dd, DWORD flags)
{
	const struct gpu *g = primary_gpu();

	if (!EnumDisplayDevicesA(device, index, dd, flags))
		return FALSE;
	/* Adapters only: a device name asks for that adapter's monitors. */
	if (g && !device) {
		WideCharToMultiByte(CP_ACP, 0, g->name, -1, dd->DeviceString,
				    sizeof(dd->DeviceString), NULL, NULL);
		if (dd->cb >= offsetof(DISPLAY_DEVICEA, DeviceKey))
			pci_id(g, dd->DeviceID, sizeof(dd->DeviceID));
	}
	return TRUE;
}

static BOOL WINAPI display_EnumDisplayDevicesW(LPCWSTR device, DWORD index,
					       DISPLAY_DEVICEW *dd, DWORD flags)
{
	const struct gpu *g = primary_gpu();
	char id[128];

	if (!EnumDisplayDevicesW(device, index, dd, flags))
		return FALSE;
	if (g && !device) {
		wcsncpy(dd->DeviceString, g->name, 127);
		dd->DeviceString[127] = 0;
		if (dd->cb >= offsetof(DISPLAY_DEVICEW, DeviceKey)) {
			pci_id(g, id, sizeof(id));
			MultiByteToWideChar(CP_ACP, 0, id, -1, dd->DeviceID,
					    128);
		}
	}
	return TRUE;
}

/* Opens the primary GPU: every display belongs to it. */
static int32_t open_primary(struct d3dkmthandle *adapter, struct winluid *luid)
{
	const struct gpu *g = primary_gpu();
	struct d3dkmt_openadapterfromluid o = { 0 };
	int32_t status;

	if (!g)
		return (int32_t)0xC00000BB;	/* NOT_SUPPORTED */
	o.adapter_luid = g->luid;
	status = kmt("D3DKMTOpenAdapterFromLuid", &o);
	if (status >= 0) {
		*adapter = o.adapter_handle;
		*luid = g->luid;
	}
	return status;
}

/* D3DKMT_OPENADAPTERFROMHDC (x64) */
struct open_from_hdc {
	HDC hdc;
	struct d3dkmthandle adapter;
	struct winluid luid;
	uint32_t vidpn_source;
};
_Static_assert(sizeof(struct open_from_hdc) == 24, "D3DKMT_OPENADAPTERFROMHDC");

int32_t WINAPI kmt_display_open_from_hdc(void *args)
{
	struct open_from_hdc *a = args;

	if (!a->hdc)
		return (int32_t)0xC000000D;	/* INVALID_PARAMETER */
	a->vidpn_source = 0;
	return open_primary(&a->adapter, &a->luid);
}

/* D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME */
struct open_from_name {
	wchar_t device_name[32];
	struct d3dkmthandle adapter;
	struct winluid luid;
	uint32_t vidpn_source;
};
_Static_assert(sizeof(struct open_from_name) == 80,
	       "D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME");

int32_t WINAPI kmt_display_open_from_name(void *args)
{
	struct open_from_name *a = args;
	unsigned n = 0;

	if (_wcsnicmp(a->device_name, L"\\\\.\\DISPLAY", 11) ||
	    swscanf(a->device_name + 11, L"%u", &n) != 1 || !n)
		return (int32_t)0xC000000D;
	a->vidpn_source = n - 1;
	return open_primary(&a->adapter, &a->luid);
}

void *kmt_display_resolve(const char *name)
{
	if (!strcmp(name, "EnumDisplayDevicesA"))
		return display_EnumDisplayDevicesA;
	if (!strcmp(name, "EnumDisplayDevicesW"))
		return display_EnumDisplayDevicesW;
	return NULL;
}
