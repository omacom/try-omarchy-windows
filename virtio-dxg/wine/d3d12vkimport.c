// SPDX-License-Identifier: MIT
/*
 * d3d12vkimport: on Windows itself, which shared D3D12 textures NVIDIA's
 * Vulkan driver imports (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT).
 * Each case creates a committed BGRA8 texture with the given flags, shares
 * it and imports it as a dedicated image allocation.
 *
 *   gcc -O2 -o d3d12vkimport.exe d3d12vkimport.c -ld3d12 -ldxgi -lvulkan-1
 */
#define COBJMACROS
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "kmt_bridge.h"

static VkDevice dev;
static VkPhysicalDevice gpu;
static PFN_vkGetMemoryWin32HandlePropertiesKHR get_props;

static VkResult import(HANDLE h, UINT w, UINT hgt, VkSharingMode sharing)
{
	VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext };
	uint32_t families[2] = { 0, 1 };
	VkImage image;
	VkMemoryRequirements req;
	VkMemoryWin32HandlePropertiesKHR hp = { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
	VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
		&ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, h };
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp };
	VkDeviceMemory mem;
	VkResult r;
	uint32_t i;

	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent.width = w;
	ici.extent.height = hgt;
	ici.extent.depth = 1;
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = sharing;
	if (sharing == VK_SHARING_MODE_CONCURRENT) {
		ici.queueFamilyIndexCount = 2;
		ici.pQueueFamilyIndices = families;
	}
	r = vkCreateImage(dev, &ici, NULL, &image);
	if (r)
		return r;
	vkGetImageMemoryRequirements(dev, image, &req);
	get_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, h, &hp);
	ded.image = image;
	mai.allocationSize = req.size;
	for (i = 0; i < 32 && !((req.memoryTypeBits & hp.memoryTypeBits) >> i & 1); i++)
		;
	mai.memoryTypeIndex = i;
	r = vkAllocateMemory(dev, &mai, NULL, &mem);
	if (!r) {
		r = vkBindImageMemory(dev, image, mem, 0);
		vkFreeMemory(dev, mem, NULL);
	}
	vkDestroyImage(dev, image, NULL);
	return r;
}

/* The kernel's view of a shared resource: sizes and the D3D12 runtime's
 * private data, for comparison with the guest's (kmt_bridge.c traces). */
struct kmt_query {
	UINT device;
	UINT reserved;
	UINT64 nt_handle;
	UINT64 runtime_data;
	UINT runtime_size;
	UINT total_driver_size;
	UINT resource_driver_size;
	UINT allocation_count;
};

static void dump_runtime_data(const uint8_t *luid, HANDLE h)
{
	HMODULE gdi = LoadLibraryA("gdi32.dll");
	LONG(WINAPI *open_adapter)(void *) = (void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
	LONG(WINAPI *create_device)(void *) = (void *)GetProcAddress(gdi, "D3DKMTCreateDevice");
	LONG(WINAPI *query)(void *) = (void *)GetProcAddress(gdi, "D3DKMTQueryResourceInfoFromNtHandle");
	struct { LUID luid; UINT adapter; } oa;
	struct { UINT64 adapter; UINT flags; UINT device; UINT64 rest[6]; } cd = { 0 };
	struct kmt_query q = { 0 };
	uint8_t data[1024];
	LONG s;
	UINT i;

	memcpy(&oa.luid, luid, sizeof(LUID));
	if (!open_adapter || (s = open_adapter(&oa))) {
		printf("  (no KMT adapter: 0x%08lx)\n", s);
		return;
	}
	cd.adapter = oa.adapter;
	if ((s = create_device(&cd))) {
		printf("  (no KMT device: 0x%08lx)\n", s);
		return;
	}
	q.device = cd.device;
	q.nt_handle = (UINT64)(uintptr_t)h;
	s = query(&q);
	printf("  native: 0x%08lx runtime data %u, driver data %u (resource %u), %u allocation(s)\n",
	       s, q.runtime_size, q.total_driver_size, q.resource_driver_size, q.allocation_count);
	{
		LONG(WINAPI *open)(void *) = (void *)GetProcAddress(gdi, "D3DKMTOpenResourceFromNtHandle");
		struct d3dkmt_openresourcefromnthandle o = { 0 };
		struct d3dddi_openallocationinfo2 alloc[4] = { 0 };
		static uint8_t driver[4096], resource[1024];

		memset(data, 0xcd, sizeof(data));
		o.device.v = cd.device;
		o.nt_handle = (UINT64)(uintptr_t)h;
		o.allocation_count = q.allocation_count;
		o.open_alloc_info = (UINT64)(uintptr_t)alloc;
		o.private_runtime_data_size = (int)q.runtime_size;
		o.private_runtime_data = (UINT64)(uintptr_t)data;
		o.resource_priv_drv_data_size = q.resource_driver_size;
		o.resource_priv_drv_data = (UINT64)(uintptr_t)resource;
		o.total_priv_drv_data_size = q.total_driver_size;
		o.total_priv_drv_data = (UINT64)(uintptr_t)driver;
		s = open(&o);
		printf("  open: 0x%08lx runtime data:", s);
		for (i = 0; i < q.runtime_size; i++)
			printf("%s%02x", i % 32 ? "" : "\n    ", data[i]);
		printf("\n");
		printf("  driver data (%u):", q.total_driver_size);
		for (i = 0; i < q.total_driver_size; i++)
			printf("%s%02x", i % 32 ? "" : "\n    ", driver[i]);
		printf("\n");
	}
}

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "d3d12vkimport", 0, NULL, 0,
				  VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
	VkInstance inst;
	VkPhysicalDevice gpus[8];
	uint32_t n = 8, i;
	VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
	const char *ext[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME };
	float prio = 1;
	VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 1, ext };
	IDXGIFactory4 *factory;
	IDXGIAdapter1 *adapter = NULL;
	ID3D12Device *d3d;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (vkCreateInstance(&ici, NULL, &inst) || vkEnumeratePhysicalDevices(inst, &n, gpus)) {
		printf("FAIL: Vulkan\n");
		return 1;
	}
	for (i = 0; i < n; i++) {
		vkGetPhysicalDeviceProperties2(gpus[i], &p2);
		if (p2.properties.vendorID == 0x10de && idp.deviceLUIDValid)
			break;
	}
	if (i == n) {
		printf("FAIL: no NVIDIA Vulkan device\n");
		return 1;
	}
	gpu = gpus[i];
	printf("Vulkan: %s\n", p2.properties.deviceName);
	if (vkCreateDevice(gpu, &dci, NULL, &dev)) {
		printf("FAIL: vkCreateDevice\n");
		return 1;
	}
	get_props = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandlePropertiesKHR");

	CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
	IDXGIFactory4_EnumAdapterByLuid(factory, *(LUID *)idp.deviceLUID, &IID_IDXGIAdapter1,
					(void **)&adapter);
	if (FAILED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
				     (void **)&d3d))) {
		printf("FAIL: D3D12CreateDevice\n");
		return 1;
	}

	static const struct {
		const char *name;
		D3D12_RESOURCE_FLAGS flags;
		VkSharingMode sharing;
	} cases[] = {
		{ "RT + simultaneous access (as Mesa), exclusive image", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, VK_SHARING_MODE_EXCLUSIVE },
		{ "RT + simultaneous access (as Mesa), concurrent image", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, VK_SHARING_MODE_CONCURRENT },
		{ "RT only, exclusive image", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, VK_SHARING_MODE_EXCLUSIVE },
		{ "RT + UAV, exclusive image", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, VK_SHARING_MODE_EXCLUSIVE },
	};
	for (i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1272, 686, 1, 1,
					     DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 },
					     D3D12_TEXTURE_LAYOUT_UNKNOWN, cases[i].flags };
		ID3D12Resource *res;
		HANDLE h = NULL;
		HRESULT hr = ID3D12Device_CreateCommittedResource(d3d, &heap, D3D12_HEAP_FLAG_SHARED, &desc,
			D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&res);

		if (SUCCEEDED(hr))
			hr = ID3D12Device_CreateSharedHandle(d3d, (ID3D12DeviceChild *)res, NULL,
							     GENERIC_ALL, NULL, &h);
		if (FAILED(hr)) {
			printf("%-55s D3D12 0x%08lx\n", cases[i].name, hr);
			continue;
		}
		printf("%-55s import: VkResult %d\n", cases[i].name,
		       import(h, 1272, 686, cases[i].sharing));
		if (i == 0)
			dump_runtime_data(idp.deviceLUID, h);
		CloseHandle(h);
		ID3D12Resource_Release(res);
	}
	return 0;
}
