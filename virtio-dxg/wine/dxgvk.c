// SPDX-License-Identifier: MIT
/*
 * dxgvk: NVIDIA's Windows Vulkan driver as a Vulkan driver for Wine and
 * Proton, on virtio-dxg (or WSL2's /dev/dxg).
 *
 * The Khronos Vulkan loader (vulkan-1.dll, native) loads this DLL as an
 * ICD. It loads nvoglv64.dll from the host's driver store with its kernel
 * calls routed to /dev/dxg (nvload.c, kmt_bridge.c) and hands out the
 * driver's own objects and entry points: everything an application calls
 * runs in NVIDIA's driver, with no translation.
 *
 * Only presentation is ours, because the driver's needs Windows' window
 * system. A surface is the Wine window's X11 window; a swapchain image is a
 * window buffer (a shared GPU texture behind a virtio-gpu dma-buf, the zero
 * copy path of the OpenGL driver) imported into the NVIDIA device, and a
 * present hands its dma-buf to the X server (DRI3 + Present), which gives
 * it to the compositor without a copy.
 *
 *   VK_DRIVER_FILES=Z:\path\dxgvk.json WINEDLLOVERRIDES=vulkan-1=n wine app.exe
 *   DXGVK_DEBUG=1 logs to stderr.
 *   DXGVK_HUD=1 shows frames per second and frame time in the top left
 *   corner of every window it presents to.
 */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include "kmt_bridge.h"
#include "dxgkmt_unix.h"
#include "nvload.h"

#define EXPORT __declspec(dllexport)
#define MAX_IMAGES 8
#define LOG(...) do { if (debug) fprintf(stderr, "dxgvk: " __VA_ARGS__); } while (0)

/* DXGVK_DEBUG logs presentation stalls longer than this. */
#define SLOW_MS 12.0

static int debug, hud;

static double now_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;

	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
static HMODULE nv;
static LUID nv_luid;
static VkResult init_result = VK_ERROR_INITIALIZATION_FAILED;
static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

static PFN_vkGetInstanceProcAddr nv_gipa;
static PFN_vkGetDeviceProcAddr nv_gdpa;
static PFN_vkVoidFunction (VKAPI_PTR *nv_gpdpa)(VkInstance, const char *);
static VkResult (VKAPI_PTR *nv_by_adapter)(VkInstance, LUID, uint32_t *,
					   VkPhysicalDevice *);

/* Instance-level driver functions (the same for every instance). */
static PFN_vkEnumeratePhysicalDevices nv_EnumeratePhysicalDevices;
static PFN_vkEnumerateDeviceExtensionProperties nv_EnumerateDeviceExtensionProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties nv_GetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties nv_GetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice nv_CreateDevice;

/* --- Loading the driver ------------------------------------------------ */

/* DXGVK_KMTSTATS=N: every N seconds, the driver's kernel calls so far, by
 * function (what a frame costs in calls to the host). */
static DWORD WINAPI kmt_stats(void *arg)
{
	DWORD seconds = (DWORD)(uintptr_t)arg, total = 0;

	for (;;) {
		Sleep(seconds * 1000);
		total += seconds;
		fprintf(stderr, "[kmt] after %lu s:\n", (unsigned long)total);
		kmt_bridge_report();
	}
	return 0;
}

static BOOL CALLBACK init(INIT_ONCE *once, void *param, void **ctx)
{
	VkResult (VKAPI_PTR *negotiate)(uint32_t *);
	wchar_t so[MAX_PATH];
	HMODULE self;
	uint32_t version = 7;

	(void)once; (void)param; (void)ctx;
	debug = getenv("DXGVK_DEBUG") ? atoi(getenv("DXGVK_DEBUG")) : 0;
	hud = getenv("DXGVK_HUD") ? atoi(getenv("DXGVK_HUD")) : 0;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)(void *)init, &self);
	GetModuleFileNameW(self, so, MAX_PATH);
	wcscpy(wcsrchr(so, L'\\') + 1, L"dxgkmt.so");
	if (nv_load(debug > 1 ? 1 : (debug ? 0 : -1), so, &nv, &nv_luid)) {
		fprintf(stderr, "dxgvk: the NVIDIA driver did not load\n");
		return TRUE;
	}
	nv_gipa = (void *)GetProcAddress(nv, "vk_icdGetInstanceProcAddr");
	nv_gpdpa = (void *)GetProcAddress(nv, "vk_icdGetPhysicalDeviceProcAddr");
	nv_by_adapter = (void *)GetProcAddress(nv, "vk_icdEnumerateAdapterPhysicalDevices");
	negotiate = (void *)GetProcAddress(nv, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (!nv_gipa || !negotiate || negotiate(&version) != VK_SUCCESS) {
		fprintf(stderr, "dxgvk: the NVIDIA driver has no usable ICD interface\n");
		return TRUE;
	}
	LOG("NVIDIA driver loaded (ICD interface %u)\n", version);
	if (getenv("DXGVK_KMTSTATS") && atoi(getenv("DXGVK_KMTSTATS")) > 0)
		CloseHandle(CreateThread(NULL, 0, kmt_stats,
			(void *)(uintptr_t)atoi(getenv("DXGVK_KMTSTATS")), 0, NULL));
	init_result = VK_SUCCESS;
	return TRUE;
}

static VkResult ensure_init(void)
{
	InitOnceExecuteOnce(&init_once, init, NULL, NULL);
	return init_result;
}

/* --- Extensions ---------------------------------------------------------- */

/* Presentation features the swapchain here does not implement. */
static const char *const hidden_extensions[] = {
	"VK_EXT_surface_maintenance1", "VK_KHR_surface_maintenance1",
	"VK_KHR_display", "VK_KHR_get_display_properties2",
	"VK_EXT_direct_mode_display", "VK_EXT_display_surface_counter",
	"VK_NV_acquire_winrt_display", "VK_KHR_surface_protected_capabilities",
	"VK_EXT_headless_surface", "VK_EXT_full_screen_exclusive",
	"VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
	"VK_KHR_present_id", "VK_KHR_present_wait", "VK_KHR_present_id2",
	"VK_KHR_present_wait2", "VK_NV_low_latency", "VK_NV_low_latency2",
	"VK_GOOGLE_display_timing", "VK_EXT_display_control",
	"VK_KHR_display_swapchain", "VK_KHR_shared_presentable_image",
	"VK_EXT_hdr_metadata", "VK_AMD_display_native_hdr",
	"VK_EXT_present_timing", "VK_NV_present_barrier",
	"VK_EXT_present_mode_fifo_latest_ready",
	"VK_KHR_present_mode_fifo_latest_ready",
	/* The optical flow engine fails device creation over virtio-dxg so far
	 * (vkd3d-proton enables it when offered). */
	"VK_NV_optical_flow",
};

static int hidden(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(hidden_extensions) / sizeof(*hidden_extensions); i++)
		if (!strcmp(name, hidden_extensions[i]))
			return 1;
	return 0;
}

static VkResult filter_extensions(VkExtensionProperties *all, uint32_t n,
				  uint32_t *count, VkExtensionProperties *out)
{
	uint32_t i, kept = 0;

	for (i = 0; i < n; i++)
		if (!hidden(all[i].extensionName))
			all[kept++] = all[i];
	if (!out) {
		*count = kept;
		return VK_SUCCESS;
	}
	if (*count > kept)
		*count = kept;
	memcpy(out, all, *count * sizeof(*out));
	return *count < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_EnumerateInstanceExtensionProperties(
	const char *layer, uint32_t *count, VkExtensionProperties *props)
{
	PFN_vkEnumerateInstanceExtensionProperties f;
	VkExtensionProperties *all;
	uint32_t n = 0;
	VkResult r;

	if (ensure_init())
		return VK_ERROR_INITIALIZATION_FAILED;
	f = (void *)nv_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
	if (layer)
		return f(layer, count, props);
	r = f(NULL, &n, NULL);
	if (r)
		return r;
	all = calloc(n + 1, sizeof(*all));
	if (!all)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	r = f(NULL, &n, all);
	if (r >= 0)
		r = filter_extensions(all, n, count, props);
	free(all);
	return r;
}

static VkResult VKAPI_CALL dxgvk_EnumerateDeviceExtensionProperties(
	VkPhysicalDevice gpu, const char *layer, uint32_t *count,
	VkExtensionProperties *props)
{
	VkExtensionProperties *all;
	uint32_t n = 0;
	VkResult r;

	if (layer)
		return nv_EnumerateDeviceExtensionProperties(gpu, layer, count, props);
	r = nv_EnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
	if (r)
		return r;
	all = calloc(n + 1, sizeof(*all));
	if (!all)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	r = nv_EnumerateDeviceExtensionProperties(gpu, NULL, &n, all);
	if (r >= 0)
		r = filter_extensions(all, n, count, props);
	free(all);
	return r;
}

/* --- Instance and physical devices -------------------------------------- */

static VkResult VKAPI_CALL dxgvk_CreateInstance(const VkInstanceCreateInfo *ci,
						const VkAllocationCallbacks *a,
						VkInstance *instance)
{
	PFN_vkCreateInstance f;
	VkResult r;

	if (ensure_init())
		return VK_ERROR_INITIALIZATION_FAILED;
	f = (void *)nv_gipa(NULL, "vkCreateInstance");
	r = f(ci, a, instance);
	if (r)
		return r;
#define NV_I(name) if (!nv_##name) nv_##name = (void *)nv_gipa(*instance, "vk" #name)
	NV_I(EnumeratePhysicalDevices);
	NV_I(EnumerateDeviceExtensionProperties);
	NV_I(GetPhysicalDeviceQueueFamilyProperties);
	NV_I(GetPhysicalDeviceMemoryProperties);
	NV_I(CreateDevice);
	if (!nv_gdpa)
		nv_gdpa = (void *)nv_gipa(*instance, "vkGetDeviceProcAddr");
	LOG("instance created (API %u.%u)\n",
	    ci->pApplicationInfo ? VK_API_VERSION_MAJOR(ci->pApplicationInfo->apiVersion) : 1,
	    ci->pApplicationInfo ? VK_API_VERSION_MINOR(ci->pApplicationInfo->apiVersion) : 0);
	return VK_SUCCESS;
}

/* The loader of a Windows driver passes the adapter; here the driver is
 * found by its adapter, so the device list is the adapter's. */
static VkResult VKAPI_CALL dxgvk_EnumeratePhysicalDevices(VkInstance instance,
							  uint32_t *count,
							  VkPhysicalDevice *gpus)
{
	VkPhysicalDevice all[8];
	uint32_t n = 8;
	VkResult r;

	if (!nv_by_adapter)
		return nv_EnumeratePhysicalDevices(instance, count, gpus);
	r = nv_by_adapter(instance, nv_luid, &n, all);
	if (r < 0)
		return r;
	if (!gpus) {
		*count = n;
		return VK_SUCCESS;
	}
	if (*count > n)
		*count = n;
	memcpy(gpus, all, *count * sizeof(*gpus));
	return *count < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* --- Devices ------------------------------------------------------------ */

struct device {
	VkDevice handle;
	VkPhysicalDevice gpu;
	VkQueue queue;			/* private: signals acquires */
	uint32_t queue_family;
	CRITICAL_SECTION queue_lock;
	/* The application's queues and their families (vkGetDeviceQueue*). */
	struct { VkQueue queue; uint32_t family; } queues[64];
	uint32_t nqueues;
	VkFence fence;			/* present waits for rendering */
	CRITICAL_SECTION fence_lock;
	VkPhysicalDeviceMemoryProperties memory;
#define DEV_FN(name) PFN_vk##name name;
	DEV_FN(GetDeviceQueue) DEV_FN(GetDeviceQueue2) DEV_FN(DestroyDevice) DEV_FN(CreateFence)
	DEV_FN(DestroyFence) DEV_FN(WaitForFences) DEV_FN(ResetFences)
	DEV_FN(QueueSubmit) DEV_FN(CreateImage) DEV_FN(DestroyImage)
	DEV_FN(GetImageMemoryRequirements) DEV_FN(AllocateMemory)
	DEV_FN(FreeMemory) DEV_FN(BindImageMemory) DEV_FN(DeviceWaitIdle)
	DEV_FN(GetMemoryWin32HandlePropertiesKHR)
	/* the frame rate overlay */
	DEV_FN(CreateBuffer) DEV_FN(DestroyBuffer) DEV_FN(GetBufferMemoryRequirements)
	DEV_FN(BindBufferMemory) DEV_FN(MapMemory) DEV_FN(CreateCommandPool)
	DEV_FN(DestroyCommandPool) DEV_FN(AllocateCommandBuffers)
	DEV_FN(BeginCommandBuffer) DEV_FN(EndCommandBuffer)
	DEV_FN(CmdPipelineBarrier) DEV_FN(CmdCopyBufferToImage)
	struct device *next;
};

static struct device *devices;
static SRWLOCK devices_lock = SRWLOCK_INIT;

static struct device *find_device(VkDevice handle)
{
	struct device *d;

	AcquireSRWLockShared(&devices_lock);
	for (d = devices; d && d->handle != handle; d = d->next)
		;
	ReleaseSRWLockShared(&devices_lock);
	return d;
}

static VkResult VKAPI_CALL dxgvk_CreateDevice(VkPhysicalDevice gpu,
					      const VkDeviceCreateInfo *ci,
					      const VkAllocationCallbacks *a,
					      VkDevice *out)
{
	VkDeviceCreateInfo info = *ci;
	VkDeviceQueueCreateInfo queues[64];
	VkQueueFamilyProperties families[32];
	const char *exts[512];
	float priorities[64];
	uint32_t nfam = 32, i, family = UINT32_MAX, index = 0, n;
	int has_import = 0;
	struct device *d;
	VkResult r;

	if (debug) {
		uint32_t q;

		LOG("vkCreateDevice: %u extensions, %u queue families:",
		    ci->enabledExtensionCount, ci->queueCreateInfoCount);
		for (q = 0; q < ci->queueCreateInfoCount; q++)
			if (debug)
				fprintf(stderr, " %u x%u", ci->pQueueCreateInfos[q].queueFamilyIndex,
					ci->pQueueCreateInfos[q].queueCount);
		if (debug)
			fprintf(stderr, "\n");
	}
	/* DXGVK_PLAIN_DEVICE=1: the application's device unchanged (no
	 * swapchain support), to tell a driver failure from ours. */
	if (getenv("DXGVK_PLAIN_DEVICE") && atoi(getenv("DXGVK_PLAIN_DEVICE"))) {
		VkResult pr = nv_CreateDevice(gpu, ci, a, out);

		LOG("plain vkCreateDevice: %d\n", pr);
		return pr;
	}
	/* DXGVK_PROBE_DEVICE=1: which queue family or extension the driver
	 * refuses, by retrying without each (diagnostics only). */
	if (getenv("DXGVK_PROBE_DEVICE") && atoi(getenv("DXGVK_PROBE_DEVICE"))) {
		VkDeviceCreateInfo t = *ci;
		VkDeviceQueueCreateInfo qs[16];
		const char *es[512];
		VkDevice probe;
		uint32_t i, j, n;

		int mode = atoi(getenv("DXGVK_PROBE_DEVICE"));

		if (mode == 1) {
			VkDeviceQueueCreateInfo one = ci->pQueueCreateInfos[0];
			VkResult pr;

			one.queueCount = 1;
			t = *ci;
			t.queueCreateInfoCount = 1;
			t.pQueueCreateInfos = &one;
			pr = nv_CreateDevice(gpu, &t, a, &probe);
			fprintf(stderr, "dxgvk: probe: one queue: %d\n", pr);
			if (pr == VK_SUCCESS)
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
			t = *ci;
			t.pNext = NULL;
			t.pEnabledFeatures = NULL;
			pr = nv_CreateDevice(gpu, &t, a, &probe);
			fprintf(stderr, "dxgvk: probe: no feature structures: %d\n", pr);
			if (pr == VK_SUCCESS)
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
			t.enabledExtensionCount = 0;
			pr = nv_CreateDevice(gpu, &t, a, &probe);
			fprintf(stderr, "dxgvk: probe: no features, no extensions: %d\n", pr);
			if (pr == VK_SUCCESS)
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
		}
		/* Each feature structure left out in turn (the chain is relinked
		 * in place and restored), then each enabled flag of one that
		 * makes the difference. */
		{
			VkBaseOutStructure *prev = (VkBaseOutStructure *)ci, *s;

			for (s = (VkBaseOutStructure *)ci->pNext; s; prev = s, s = s->pNext) {
				VkResult pr;

				prev->pNext = s->pNext;
				pr = nv_CreateDevice(gpu, ci, a, &probe);
				prev->pNext = s;
				fprintf(stderr, "dxgvk: probe: without sType %d: %d\n", s->sType, pr);
				if (pr != VK_SUCCESS)
					continue;
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
				fprintf(stderr, "dxgvk: probe: works without feature structure sType %d\n",
					s->sType);
				{
					VkBool32 *flag = (VkBool32 *)((char *)s + sizeof(VkBaseOutStructure));
					uint32_t k;

					/* Feature structures are VkBool32 arrays after the
					 * header; 512 bounds VkPhysicalDeviceFeatures2. */
					for (k = 0; k < 128; k++) {
						if (flag[k] != VK_TRUE)
							continue;
						flag[k] = VK_FALSE;
						pr = nv_CreateDevice(gpu, ci, a, &probe);
						flag[k] = VK_TRUE;
						if (pr == VK_SUCCESS) {
							((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
							fprintf(stderr, "dxgvk: probe:   ... and without its flag %u\n", k);
						}
					}
				}
			}
		}
		for (i = 0; mode == 1 && i < ci->queueCreateInfoCount && ci->queueCreateInfoCount <= 16; i++) {
			for (j = 0, n = 0; j < ci->queueCreateInfoCount; j++)
				if (j != i)
					qs[n++] = ci->pQueueCreateInfos[j];
			t = *ci;
			t.queueCreateInfoCount = n;
			t.pQueueCreateInfos = qs;
			if (nv_CreateDevice(gpu, &t, a, &probe) == VK_SUCCESS) {
				fprintf(stderr, "dxgvk: probe: works without queue family %u\n",
					ci->pQueueCreateInfos[i].queueFamilyIndex);
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
			}
		}
		for (i = 0; mode == 1 && i < ci->enabledExtensionCount && ci->enabledExtensionCount <= 512; i++) {
			for (j = 0, n = 0; j < ci->enabledExtensionCount; j++)
				if (j != i)
					es[n++] = ci->ppEnabledExtensionNames[j];
			t = *ci;
			t.enabledExtensionCount = n;
			t.ppEnabledExtensionNames = es;
			if (nv_CreateDevice(gpu, &t, a, &probe) == VK_SUCCESS) {
				fprintf(stderr, "dxgvk: probe: works without %s\n",
					ci->ppEnabledExtensionNames[i]);
				((PFN_vkDestroyDevice)nv_gdpa(probe, "vkDestroyDevice"))(probe, a);
			}
		}
	}
	if (ci->queueCreateInfoCount > 63 || ci->enabledExtensionCount > 510)
		return nv_CreateDevice(gpu, ci, a, out);
	/* The swapchain imports window buffers into the device. */
	for (i = 0; i < ci->enabledExtensionCount; i++) {
		exts[i] = ci->ppEnabledExtensionNames[i];
		has_import |= !strcmp(exts[i], VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	}
	n = ci->enabledExtensionCount;
	if (!has_import)
		exts[n++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
	info.enabledExtensionCount = n;
	info.ppEnabledExtensionNames = exts;

	/* One queue of our own for acquires: one more of a family the
	 * application uses, or one of a family it does not. */
	nv_GetPhysicalDeviceQueueFamilyProperties(gpu, &nfam, families);
	memcpy(queues, ci->pQueueCreateInfos, ci->queueCreateInfoCount * sizeof(*queues));
	for (i = 0; i < ci->queueCreateInfoCount && family == UINT32_MAX; i++) {
		VkDeviceQueueCreateInfo *q = &queues[i];

		if (q->queueFamilyIndex < nfam && !q->flags &&
		    q->queueCount < families[q->queueFamilyIndex].queueCount &&
		    q->queueCount < 63) {
			memcpy(priorities, q->pQueuePriorities, q->queueCount * sizeof(float));
			priorities[q->queueCount] = 1.0f;
			family = q->queueFamilyIndex;
			index = q->queueCount++;
			q->pQueuePriorities = priorities;
		}
	}
	for (i = 0; i < nfam && family == UINT32_MAX; i++) {
		uint32_t j, used = 0;

		for (j = 0; j < ci->queueCreateInfoCount; j++)
			used |= ci->pQueueCreateInfos[j].queueFamilyIndex == i;
		if (!used && families[i].queueCount) {
			static const float one = 1.0f;
			VkDeviceQueueCreateInfo *q = &queues[ci->queueCreateInfoCount];

			memset(q, 0, sizeof(*q));
			q->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			q->queueFamilyIndex = family = i;
			q->queueCount = 1;
			q->pQueuePriorities = &one;
			info.queueCreateInfoCount = ci->queueCreateInfoCount + 1;
		}
	}
	info.pQueueCreateInfos = queues;
	r = nv_CreateDevice(gpu, &info, a, out);
	if (r) {
		LOG("vkCreateDevice: %d\n", r);
		return r;
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return VK_SUCCESS;	/* rendering works; presenting will not */
	d->handle = *out;
	d->gpu = gpu;
#define LOAD(name) d->name = (void *)nv_gdpa(*out, "vk" #name)
	LOAD(GetDeviceQueue); LOAD(GetDeviceQueue2); LOAD(DestroyDevice); LOAD(CreateFence);
	LOAD(DestroyFence); LOAD(WaitForFences); LOAD(ResetFences);
	LOAD(QueueSubmit); LOAD(CreateImage); LOAD(DestroyImage);
	LOAD(GetImageMemoryRequirements); LOAD(AllocateMemory);
	LOAD(FreeMemory); LOAD(BindImageMemory); LOAD(DeviceWaitIdle);
	LOAD(GetMemoryWin32HandlePropertiesKHR);
	LOAD(CreateBuffer); LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements);
	LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(CreateCommandPool);
	LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers);
	LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer);
	LOAD(CmdPipelineBarrier); LOAD(CmdCopyBufferToImage);
	nv_GetPhysicalDeviceMemoryProperties(gpu, &d->memory);
	d->queue_family = family;
	if (family != UINT32_MAX)
		d->GetDeviceQueue(*out, family, index, &d->queue);
	InitializeCriticalSection(&d->queue_lock);
	InitializeCriticalSection(&d->fence_lock);
	{
		VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

		d->CreateFence(*out, &fi, NULL, &d->fence);
	}
	AcquireSRWLockExclusive(&devices_lock);
	d->next = devices;
	devices = d;
	ReleaseSRWLockExclusive(&devices_lock);
	LOG("device created; acquire queue: family %d index %u\n", (int)family, index);
	return VK_SUCCESS;
}

static void remember_queue(struct device *d, VkQueue queue, uint32_t family)
{
	uint32_t i;

	EnterCriticalSection(&d->queue_lock);
	for (i = 0; i < d->nqueues && d->queues[i].queue != queue; i++)
		;
	if (i == d->nqueues && i < sizeof(d->queues) / sizeof(d->queues[0])) {
		d->queues[i].queue = queue;
		d->queues[i].family = family;
		d->nqueues++;
	}
	LeaveCriticalSection(&d->queue_lock);
}

/* The family of an application queue, UINT32_MAX if unknown. */
static uint32_t queue_family(struct device *d, VkQueue queue)
{
	uint32_t i, family = UINT32_MAX;

	EnterCriticalSection(&d->queue_lock);
	for (i = 0; i < d->nqueues; i++)
		if (d->queues[i].queue == queue)
			family = d->queues[i].family;
	LeaveCriticalSection(&d->queue_lock);
	return family;
}

static void VKAPI_CALL dxgvk_GetDeviceQueue(VkDevice device, uint32_t family,
					    uint32_t index, VkQueue *queue)
{
	struct device *d = find_device(device);
	PFN_vkGetDeviceQueue get = d ? d->GetDeviceQueue :
		(PFN_vkGetDeviceQueue)nv_gdpa(device, "vkGetDeviceQueue");

	get(device, family, index, queue);
	if (d && *queue)
		remember_queue(d, *queue, family);
}

static void VKAPI_CALL dxgvk_GetDeviceQueue2(VkDevice device,
					     const VkDeviceQueueInfo2 *info,
					     VkQueue *queue)
{
	struct device *d = find_device(device);
	PFN_vkGetDeviceQueue2 get = d ? d->GetDeviceQueue2 :
		(PFN_vkGetDeviceQueue2)nv_gdpa(device, "vkGetDeviceQueue2");

	get(device, info, queue);
	if (d && *queue)
		remember_queue(d, *queue, info->queueFamilyIndex);
}

static void VKAPI_CALL dxgvk_DestroyDevice(VkDevice handle, const VkAllocationCallbacks *a)
{
	struct device **p, *d = NULL;
	PFN_vkDestroyDevice destroy;

	AcquireSRWLockExclusive(&devices_lock);
	for (p = &devices; *p; p = &(*p)->next)
		if ((*p)->handle == handle) {
			d = *p;
			*p = d->next;
			break;
		}
	ReleaseSRWLockExclusive(&devices_lock);
	if (!d) {
		destroy = (void *)nv_gdpa(handle, "vkDestroyDevice");
		destroy(handle, a);
		return;
	}
	d->DestroyFence(handle, d->fence, NULL);
	d->DestroyDevice(handle, a);
	DeleteCriticalSection(&d->queue_lock);
	DeleteCriticalSection(&d->fence_lock);
	free(d);
}

/* --- Surfaces ------------------------------------------------------------ */

/* Starts like the loader's VkIcdSurfaceWin32, which older loaders pass. */
struct surface {
	VkIcdSurfaceWin32 base;
};

static struct surface *surface_of(VkSurfaceKHR handle)
{
	return (struct surface *)(uintptr_t)handle;
}

static VkResult VKAPI_CALL dxgvk_CreateWin32SurfaceKHR(VkInstance instance,
	const VkWin32SurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a,
	VkSurfaceKHR *out)
{
	struct surface *s = calloc(1, sizeof(*s));

	(void)instance; (void)a;
	if (!s)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->base.base.platform = VK_ICD_WSI_PLATFORM_WIN32;
	s->base.hinstance = ci->hinstance;
	s->base.hwnd = ci->hwnd;
	*out = (VkSurfaceKHR)(uintptr_t)s;
	LOG("surface for window %p\n", (void *)ci->hwnd);
	return VK_SUCCESS;
}

static void VKAPI_CALL dxgvk_DestroySurfaceKHR(VkInstance instance,
					       VkSurfaceKHR surface,
					       const VkAllocationCallbacks *a)
{
	(void)instance; (void)a;
	free(surface_of(surface));
}

static VkExtent2D client_size(HWND hwnd)
{
	RECT r = { 0 };
	VkExtent2D e;

	GetClientRect(hwnd, &r);
	e.width = (uint32_t)(r.right - r.left);
	e.height = (uint32_t)(r.bottom - r.top);
	return e;
}

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfaceSupportKHR(
	VkPhysicalDevice gpu, uint32_t family, VkSurfaceKHR surface, VkBool32 *ok)
{
	(void)gpu; (void)family; (void)surface;
	*ok = VK_TRUE;
	return VK_SUCCESS;
}

static VkBool32 VKAPI_CALL dxgvk_GetPhysicalDeviceWin32PresentationSupportKHR(
	VkPhysicalDevice gpu, uint32_t family)
{
	(void)gpu; (void)family;
	return VK_TRUE;
}

#define SWAPCHAIN_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |		\
			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |		\
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT |		\
			 VK_IMAGE_USAGE_SAMPLED_BIT)

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice gpu, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *c)
{
	VkExtent2D e = client_size(surface_of(surface)->base.hwnd);

	(void)gpu;
	memset(c, 0, sizeof(*c));
	c->minImageCount = 2;
	c->maxImageCount = MAX_IMAGES;
	c->currentExtent = e;
	c->minImageExtent.width = c->minImageExtent.height = 1;
	c->maxImageExtent.width = c->maxImageExtent.height = 16384;
	c->maxImageArrayLayers = 1;
	c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	c->supportedUsageFlags = SWAPCHAIN_USAGE;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfaceCapabilities2KHR(
	VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR *info,
	VkSurfaceCapabilities2KHR *c)
{
	return dxgvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, info->surface,
							     &c->surfaceCapabilities);
}

/* Window buffers are BGRA (DRM ARGB8888/XRGB8888): D3D12 opens them in
 * that format, with no sRGB view of it. */
static const VkSurfaceFormatKHR formats[] = {
	{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfaceFormatsKHR(
	VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count,
	VkSurfaceFormatKHR *out)
{
	uint32_t n = sizeof(formats) / sizeof(*formats);

	(void)gpu; (void)surface;
	if (!out) {
		*count = n;
		return VK_SUCCESS;
	}
	if (*count > n)
		*count = n;
	memcpy(out, formats, *count * sizeof(*out));
	return *count < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfaceFormats2KHR(
	VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR *info,
	uint32_t *count, VkSurfaceFormat2KHR *out)
{
	uint32_t n = sizeof(formats) / sizeof(*formats), i;

	(void)gpu; (void)info;
	if (!out) {
		*count = n;
		return VK_SUCCESS;
	}
	if (*count > n)
		*count = n;
	for (i = 0; i < *count; i++)
		out[i].surfaceFormat = formats[i];
	return *count < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static const VkPresentModeKHR modes[] = {
	VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
	VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static VkResult VKAPI_CALL dxgvk_GetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count,
	VkPresentModeKHR *out)
{
	uint32_t n = sizeof(modes) / sizeof(*modes);

	(void)gpu; (void)surface;
	if (!out) {
		*count = n;
		return VK_SUCCESS;
	}
	if (*count > n)
		*count = n;
	memcpy(out, modes, *count * sizeof(*out));
	return *count < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_GetPhysicalDevicePresentRectanglesKHR(
	VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count, VkRect2D *out)
{
	(void)gpu;
	if (!out) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (!*count)
		return VK_INCOMPLETE;
	*count = 1;
	out->offset.x = out->offset.y = 0;
	out->extent = client_size(surface_of(surface)->base.hwnd);
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_GetDeviceGroupSurfacePresentModesKHR(
	VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *m)
{
	(void)device; (void)surface;
	*m = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_GetDeviceGroupPresentCapabilitiesKHR(
	VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *c)
{
	(void)device;
	memset(c->presentMask, 0, sizeof(c->presentMask));
	c->presentMask[0] = 1;
	c->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
	return VK_SUCCESS;
}

/* --- Swapchains ---------------------------------------------------------- */

struct image {
	VkImage image;
	VkDeviceMemory memory;
	int dmabuf_fd, resource_fd;	/* Linux descriptors (dxgkmt.so) */
	uint64_t bo;			/* the allocation holding the texture */
	uint32_t pixmap;
};

/* The frame rate overlay: text drawn on the CPU into a small buffer, copied
 * into each image by a command buffer that runs in the present's own
 * submission (recorded once per image for the presenting queue's family). */
struct hud {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t *pixels;
	VkCommandPool pool;
	VkCommandBuffer cmds[MAX_IMAGES];
	uint32_t family;		/* of the pool; UINT32_MAX until recorded */
	uint32_t width, height;
	LARGE_INTEGER freq, since, last;
	uint32_t frames;
	double longest;			/* seconds, the worst frame this window */
};

struct swapchain {
	struct device *dev;
	HWND hwnd;
	uint32_t x_window;
	VkExtent2D extent;
	VkPresentModeKHR mode;
	uint32_t count, next, stride, depth;
	int32_t x, y;			/* client area in the X window */
	int located;
	struct image images[MAX_IMAGES];
	struct hud *hud;
	/* Presentation runs on the swapchain's own thread: vkQueuePresentKHR
	 * queues the frame and returns, so the application records the next
	 * frame while the GPU renders this one. The thread waits for the
	 * image's fence (rendering done), then hands the image to X. */
	HANDLE thread;
	CRITICAL_SECTION lock;
	CONDITION_VARIABLE cond;
	VkFence fences[MAX_IMAGES];
	uint32_t pending[MAX_IMAGES];	/* image indices, in present order */
	uint32_t first, queued;
	int busy[MAX_IMAGES];		/* queued or being presented */
	VkResult status;		/* a failed present, reported next */
	int stop;
	double last_present;		/* ms, for DXGVK_DEBUG's stall log */
};

/* 5x7 glyphs, one byte per row, the leftmost pixel in bit 4. */
static const struct {
	char c;
	unsigned char rows[7];
} glyphs[] = {
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
};

#define HUD_CHARS 28
#define HUD_SCALE 2
#define HUD_ADVANCE (6 * HUD_SCALE)
#define HUD_PAD 4
#define HUD_MARGIN 8

static void hud_text(struct hud *h, const char *text)
{
	uint32_t x, y, i, n;

	for (i = 0; i < h->width * h->height; i++)
		h->pixels[i] = 0xff181818;	/* B8G8R8A8: dark grey */
	for (n = 0; text[n] && n < HUD_CHARS; n++) {
		const unsigned char *rows = NULL;

		for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
			if (glyphs[i].c == text[n])
				rows = glyphs[i].rows;
		if (!rows)
			continue;
		for (y = 0; y < 7 * HUD_SCALE; y++)
			for (x = 0; x < 5 * HUD_SCALE; x++) {
				uint32_t px = HUD_PAD + n * HUD_ADVANCE + x, py = HUD_PAD + y;

				if ((rows[y / HUD_SCALE] >> (4 - x / HUD_SCALE)) & 1 &&
				    px < h->width && py < h->height)
					h->pixels[py * h->width + px] = 0xff5af078;	/* green */
			}
	}
}

static void hud_destroy(struct swapchain *sc)
{
	struct device *d = sc->dev;
	struct hud *h = sc->hud;

	if (!h)
		return;
	if (h->pool)
		d->DestroyCommandPool(d->handle, h->pool, NULL);
	if (h->buffer)
		d->DestroyBuffer(d->handle, h->buffer, NULL);
	if (h->memory)
		d->FreeMemory(d->handle, h->memory, NULL);
	free(h);
	sc->hud = NULL;
}

static void hud_create(struct swapchain *sc)
{
	struct device *d = sc->dev;
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkMemoryRequirements req;
	struct hud *h;
	uint32_t i;

	if (!hud || !d->CmdCopyBufferToImage)
		return;
	h = sc->hud = calloc(1, sizeof(*h));
	if (!h)
		return;
	h->family = UINT32_MAX;
	h->width = HUD_CHARS * HUD_ADVANCE + 2 * HUD_PAD;
	h->height = 7 * HUD_SCALE + 2 * HUD_PAD;
	if (h->width + HUD_MARGIN > sc->extent.width ||
	    h->height + HUD_MARGIN > sc->extent.height)
		goto fail;
	bci.size = (VkDeviceSize)h->width * h->height * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (d->CreateBuffer(d->handle, &bci, NULL, &h->buffer))
		goto fail;
	d->GetBufferMemoryRequirements(d->handle, h->buffer, &req);
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = UINT32_MAX;
	for (i = 0; i < d->memory.memoryTypeCount && mai.memoryTypeIndex == UINT32_MAX; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (d->memory.memoryTypes[i].propertyFlags & host) == host)
			mai.memoryTypeIndex = i;
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    d->AllocateMemory(d->handle, &mai, NULL, &h->memory) ||
	    d->BindBufferMemory(d->handle, h->buffer, h->memory, 0) ||
	    d->MapMemory(d->handle, h->memory, 0, VK_WHOLE_SIZE, 0, (void **)&h->pixels))
		goto fail;
	QueryPerformanceFrequency(&h->freq);
	QueryPerformanceCounter(&h->since);
	hud_text(h, "    FPS");
	return;
fail:
	LOG("frame rate overlay unavailable\n");
	hud_destroy(sc);
}

/* The copy into each image, for queues of one family: 0, or -1 when the
 * application presents from another family than the first (no overlay). */
static int hud_record(struct swapchain *sc, uint32_t family)
{
	struct device *d = sc->dev;
	struct hud *h = sc->hud;
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	uint32_t i;

	if (h->family == family)
		return 0;
	if (h->family != UINT32_MAX || family == UINT32_MAX)
		return -1;
	h->family = UINT32_MAX - 1;	/* tried: no second attempt */
	pci.queueFamilyIndex = family;
	if (d->CreateCommandPool(d->handle, &pci, NULL, &h->pool))
		return -1;
	cai.commandPool = h->pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = sc->count;
	if (d->AllocateCommandBuffers(d->handle, &cai, h->cmds))
		return -1;
	for (i = 0; i < sc->count; i++) {
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		VkBufferImageCopy copy = { 0 };
		VkCommandBuffer cmd = h->cmds[i];

		/* Dispatchable: the driver's command buffer starts with the
		 * loader's dispatch pointer, which the device's provides. */
		*(void **)cmd = *(void **)d->handle;
		if (d->BeginCommandBuffer(cmd, &begin))
			return -1;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = sc->images[i].image;
		b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.subresourceRange.levelCount = 1;
		b.subresourceRange.layerCount = 1;
		d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset.x = HUD_MARGIN;
		copy.imageOffset.y = HUD_MARGIN;
		copy.imageExtent.width = h->width;
		copy.imageExtent.height = h->height;
		copy.imageExtent.depth = 1;
		d->CmdCopyBufferToImage(cmd, h->buffer, b.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
		if (d->EndCommandBuffer(cmd))
			return -1;
	}
	h->family = family;
	return 0;
}

/* Counts a frame; twice a second the text gets the new rate, the average
 * frame time and the longest frame (what a stutter shows as). */
static void hud_frame(struct hud *h)
{
	LARGE_INTEGER now;
	double seconds, frame;

	QueryPerformanceCounter(&now);
	if (h->last.QuadPart) {
		frame = (double)(now.QuadPart - h->last.QuadPart) / (double)h->freq.QuadPart;
		if (frame > h->longest)
			h->longest = frame;
	}
	h->last = now;
	h->frames++;
	seconds = (double)(now.QuadPart - h->since.QuadPart) / (double)h->freq.QuadPart;
	if (seconds >= 0.5) {
		char text[40];

		snprintf(text, sizeof(text), "%4.0f FPS %5.2f MS MAX %5.1f",
			 h->frames / seconds, seconds * 1000.0 / h->frames,
			 h->longest * 1000.0);
		hud_text(h, text);
		LOG("%s\n", text);
		h->frames = 0;
		h->longest = 0;
		h->since = now;
	}
}

static uint32_t x_window_of(HWND hwnd)
{
	uint32_t w = (uint32_t)(uintptr_t)GetPropA(hwnd, "__wine_x11_client_window");

	if (!w)
		w = (uint32_t)(uintptr_t)GetPropA(hwnd, "__wine_x11_whole_window");
	return w;
}

static void close_fd(int fd)
{
	struct dxgkmt_fd a = { fd, 0 };

	if (fd >= 0)
		kmt_bridge_call(DXGKMT_UNIX_CLOSE, &a);
}

static void destroy_images(struct swapchain *sc)
{
	uint32_t i;

	for (i = 0; i < sc->count; i++) {
		struct image *im = &sc->images[i];
		struct dxgkmt_present p = { 0 };

		if (im->pixmap) {
			p.pixmap = im->pixmap;
			kmt_bridge_call(DXGKMT_UNIX_X_FREE, &p);
		}
		if (im->image)
			sc->dev->DestroyImage(sc->dev->handle, im->image, NULL);
		if (im->memory)
			sc->dev->FreeMemory(sc->dev->handle, im->memory, NULL);
		close_fd(im->dmabuf_fd);
		close_fd(im->resource_fd);
		if (im->bo) {
			struct dxgkmt_winbuf wb = { 0 };

			wb.bo = im->bo;
			kmt_bridge_call(DXGKMT_UNIX_WINBUF_DESTROY, &wb);
		}
	}
}

static uint32_t device_local_type(struct device *d, uint32_t bits)
{
	uint32_t i;

	for (i = 0; i < d->memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d->memory.memoryTypes[i].propertyFlags &
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			return i;
	for (i = 0; i < 32; i++)
		if (bits & (1u << i))
			return i;
	return 0;
}

static VkResult create_image(struct swapchain *sc, struct image *im,
			     const VkSwapchainCreateInfoKHR *ci)
{
	struct device *d = sc->dev;
	struct dxgkmt_winbuf wb = { 0 };
	VkExternalMemoryImageCreateInfo ext = {
		VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext };
	VkMemoryDedicatedAllocateInfo dedicated = {
		VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkImportMemoryWin32HandleInfoKHR import = {
		VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
	VkMemoryRequirements req;
	VkResult r;

	im->dmabuf_fd = im->resource_fd = -1;
	wb.width = sc->extent.width;
	wb.height = sc->extent.height;
	wb.x_window = sc->x_window;
	if (kmt_bridge_call(DXGKMT_UNIX_WINBUF_CREATE, &wb) || wb.ret < 0) {
		LOG("window buffer %ux%u: error %lld\n", wb.width, wb.height,
		    (long long)wb.ret);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	im->dmabuf_fd = wb.dmabuf_fd;
	im->resource_fd = wb.resource_fd;
	im->bo = wb.bo;
	sc->stride = wb.stride;
	sc->depth = wb.depth;

	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent.width = sc->extent.width;
	ici.extent.height = sc->extent.height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = ci->imageUsage & SWAPCHAIN_USAGE;
	ici.sharingMode = ci->imageSharingMode;
	ici.queueFamilyIndexCount = ci->queueFamilyIndexCount;
	ici.pQueueFamilyIndices = ci->pQueueFamilyIndices;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	/* The texture allows simultaneous access (D3D12's form of concurrent
	 * sharing): DXGVK_CONCURRENT=1 creates the image to match. */
	if (getenv("DXGVK_CONCURRENT") && atoi(getenv("DXGVK_CONCURRENT"))) {
		static const uint32_t families[] = { 0, 1, 2 };

		ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
		ici.queueFamilyIndexCount = 3;
		ici.pQueueFamilyIndices = families;
	}
	r = d->CreateImage(d->handle, &ici, NULL, &im->image);
	if (r) {
		LOG("vkCreateImage: %d\n", r);
		return r;
	}
	d->GetImageMemoryRequirements(d->handle, im->image, &req);
	dedicated.image = im->image;
	/* A dxgkrnl NT handle is a descriptor on Linux; the driver holds it as
	 * a marked handle that its OpenResourceFromNtHandle turns back. */
	import.handle = kmt_bridge_fd_handle(im->resource_fd);
	mai.allocationSize = req.size;
	{
		VkMemoryWin32HandlePropertiesKHR hp = {
			VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		VkResult hr = d->GetMemoryWin32HandlePropertiesKHR ?
			d->GetMemoryWin32HandlePropertiesKHR(d->handle,
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
				import.handle, &hp) : VK_ERROR_EXTENSION_NOT_PRESENT;
		uint32_t bits = req.memoryTypeBits;

		if (hr == VK_SUCCESS && (bits & hp.memoryTypeBits))
			bits &= hp.memoryTypeBits;
		mai.memoryTypeIndex = device_local_type(d, bits);
		if (getenv("DXGVK_MEMTYPE"))
			mai.memoryTypeIndex = (uint32_t)atoi(getenv("DXGVK_MEMTYPE"));
		LOG("image %ux%u: size %llu, types 0x%x, handle types 0x%x (%d), using %u\n",
		    ici.extent.width, ici.extent.height, (unsigned long long)req.size,
		    req.memoryTypeBits, hp.memoryTypeBits, hr, mai.memoryTypeIndex);
	}
	r = d->AllocateMemory(d->handle, &mai, NULL, &im->memory);
	if (r) {
		LOG("importing the window buffer: vkAllocateMemory %d\n", r);
		return r;
	}
	r = d->BindImageMemory(d->handle, im->image, im->memory, 0);
	if (r)
		LOG("vkBindImageMemory: %d\n", r);
	return r;
}

static DWORD WINAPI present_thread(void *arg);

/* A fence per image and the presenting thread. */
static VkResult start_presenting(struct swapchain *sc)
{
	struct device *d = sc->dev;
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	uint32_t i;

	InitializeCriticalSection(&sc->lock);
	InitializeConditionVariable(&sc->cond);
	for (i = 0; i < sc->count; i++)
		if (d->CreateFence(d->handle, &fci, NULL, &sc->fences[i]))
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	sc->thread = CreateThread(NULL, 0, present_thread, sc, 0, NULL);
	if (!sc->thread)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	SetThreadPriority(sc->thread, THREAD_PRIORITY_ABOVE_NORMAL);
	return VK_SUCCESS;
}

static void stop_presenting(struct swapchain *sc)
{
	struct device *d = sc->dev;
	uint32_t i;

	if (sc->thread) {
		EnterCriticalSection(&sc->lock);
		sc->stop = 1;
		WakeAllConditionVariable(&sc->cond);
		LeaveCriticalSection(&sc->lock);
		WaitForSingleObject(sc->thread, INFINITE);
		CloseHandle(sc->thread);
		sc->thread = NULL;
	}
	d->DeviceWaitIdle(d->handle);
	for (i = 0; i < sc->count; i++)
		if (sc->fences[i])
			d->DestroyFence(d->handle, sc->fences[i], NULL);
	DeleteCriticalSection(&sc->lock);
}

static VkResult VKAPI_CALL dxgvk_CreateSwapchainKHR(VkDevice device,
	const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a,
	VkSwapchainKHR *out)
{
	struct device *d = find_device(device);
	struct swapchain *sc;
	uint32_t i;
	VkResult r = VK_SUCCESS;

	(void)a;
	if (!d)
		return VK_ERROR_INITIALIZATION_FAILED;
	sc = calloc(1, sizeof(*sc));
	if (!sc)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	sc->dev = d;
	sc->hwnd = surface_of(ci->surface)->base.hwnd;
	sc->x_window = x_window_of(sc->hwnd);
	sc->extent = ci->imageExtent;
	sc->mode = ci->presentMode;
	sc->count = ci->minImageCount < 2 ? 2 : ci->minImageCount + 1;
	if (sc->count > MAX_IMAGES)
		sc->count = MAX_IMAGES;
	if (!sc->x_window) {
		LOG("window %p has no X11 window\n", (void *)sc->hwnd);
		free(sc);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	for (i = 0; i < sc->count && !r; i++)
		r = create_image(sc, &sc->images[i], ci);
	if (r) {
		destroy_images(sc);
		free(sc);
		return r;
	}
	LOG("swapchain %ux%u, %u images, present mode %d, X window 0x%x\n",
	    sc->extent.width, sc->extent.height, sc->count, sc->mode, sc->x_window);
	hud_create(sc);
	r = start_presenting(sc);
	if (r) {
		stop_presenting(sc);
		hud_destroy(sc);
		destroy_images(sc);
		free(sc);
		return r;
	}
	*out = (VkSwapchainKHR)(uintptr_t)sc;
	return VK_SUCCESS;
}

static void VKAPI_CALL dxgvk_DestroySwapchainKHR(VkDevice device,
						 VkSwapchainKHR handle,
						 const VkAllocationCallbacks *a)
{
	struct swapchain *sc = (struct swapchain *)(uintptr_t)handle;

	(void)device; (void)a;
	if (!sc)
		return;
	stop_presenting(sc);
	hud_destroy(sc);
	destroy_images(sc);
	free(sc);
}

static VkResult VKAPI_CALL dxgvk_GetSwapchainImagesKHR(VkDevice device,
	VkSwapchainKHR handle, uint32_t *count, VkImage *out)
{
	struct swapchain *sc = (struct swapchain *)(uintptr_t)handle;
	uint32_t i;

	(void)device;
	if (!out) {
		*count = sc->count;
		return VK_SUCCESS;
	}
	if (*count > sc->count)
		*count = sc->count;
	for (i = 0; i < *count; i++)
		out[i] = sc->images[i].image;
	return *count < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static int resized(struct swapchain *sc)
{
	VkExtent2D e = client_size(sc->hwnd);

	return e.width && e.height &&
	       (e.width != sc->extent.width || e.height != sc->extent.height);
}

static VkResult VKAPI_CALL dxgvk_AcquireNextImageKHR(VkDevice device,
	VkSwapchainKHR handle, uint64_t timeout, VkSemaphore semaphore,
	VkFence fence, uint32_t *index)
{
	struct swapchain *sc = (struct swapchain *)(uintptr_t)handle;
	struct device *d = sc->dev;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkResult r = VK_SUCCESS;

	double t0 = now_ms(), waited;

	(void)device;
	*index = sc->next;
	/* The image may still be on its way to the screen. */
	EnterCriticalSection(&sc->lock);
	while (sc->busy[*index] && sc->status >= 0) {
		DWORD ms = timeout == UINT64_MAX ? INFINITE :
			   timeout / 1000000 >= INFINITE ? INFINITE - 1 :
			   (DWORD)(timeout / 1000000);

		if (!timeout || !SleepConditionVariableCS(&sc->cond, &sc->lock, ms)) {
			LeaveCriticalSection(&sc->lock);
			return timeout ? VK_TIMEOUT : VK_NOT_READY;
		}
	}
	r = sc->status;
	LeaveCriticalSection(&sc->lock);
	/* FIFO: the X server holds a presented image until a later one is on
	 * screen. At most 100 ms: a window that is not shown gets no vblanks. */
	if (r >= 0 && sc->images[*index].pixmap &&
	    (sc->mode == VK_PRESENT_MODE_FIFO_KHR ||
	     sc->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
		struct dxgkmt_idle idle = { sc->x_window, sc->images[*index].pixmap,
					    timeout ? 100 : 0, 0 };

		kmt_bridge_call(DXGKMT_UNIX_X_IDLE, &idle);
		if (!idle.ret && !timeout)
			return VK_NOT_READY;
	}
	waited = now_ms() - t0;
	if (debug && waited > SLOW_MS)
		LOG("slow acquire: %.1f ms waiting for image %u\n", waited, *index);
	if (r < 0)
		return r;
	sc->next = (sc->next + 1) % sc->count;
	/* The image is free when acquired: signal at once, in order. */
	if (semaphore || fence) {
		if (!d->queue)
			return VK_ERROR_DEVICE_LOST;
		si.signalSemaphoreCount = semaphore ? 1 : 0;
		si.pSignalSemaphores = &semaphore;
		EnterCriticalSection(&d->queue_lock);
		r = d->QueueSubmit(d->queue, 1, &si, fence);
		LeaveCriticalSection(&d->queue_lock);
	}
	if (r)
		return r;
	return resized(sc) ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

static VkResult VKAPI_CALL dxgvk_AcquireNextImage2KHR(VkDevice device,
	const VkAcquireNextImageInfoKHR *info, uint32_t *index)
{
	return dxgvk_AcquireNextImageKHR(device, info->swapchain, info->timeout,
					 info->semaphore, info->fence, index);
}

static VkResult present_one(struct swapchain *sc, uint32_t index)
{
	struct image *im = &sc->images[index];
	struct dxgkmt_present p = { 0 };

	p.x_window = sc->x_window;
	p.pixmap = im->pixmap;
	p.dmabuf_fd = im->dmabuf_fd;
	p.width = sc->extent.width;
	p.height = sc->extent.height;
	p.stride = sc->stride;
	p.depth = sc->depth;
	p.wait = sc->mode == VK_PRESENT_MODE_FIFO_KHR ||
		 sc->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	if (!sc->located) {
		POINT origin = { 0, 0 };

		ClientToScreen(sc->hwnd, &origin);
		p.root_x = origin.x;
		p.root_y = origin.y;
		p.locate = 1;
	} else {
		p.x = sc->x;
		p.y = sc->y;
	}
	if (kmt_bridge_call(DXGKMT_UNIX_X_PRESENT, &p) || p.ret < 0) {
		LOG("present: error %lld\n", (long long)p.ret);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	if (p.locate) {
		sc->x = p.x;
		sc->y = p.y;
		sc->located = 1;
		LOG("client area at %d,%d of the X window\n", p.x, p.y);
	}
	im->pixmap = p.pixmap;
	return resized(sc) ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

/* Waits for each queued image's rendering, then presents it, in order. */
static DWORD WINAPI present_thread(void *arg)
{
	struct swapchain *sc = arg;
	struct device *d = sc->dev;

	for (;;) {
		uint32_t index;
		double t0, t1, t2;
		VkResult r;

		EnterCriticalSection(&sc->lock);
		while (!sc->queued && !sc->stop)
			SleepConditionVariableCS(&sc->cond, &sc->lock, INFINITE);
		if (sc->stop) {
			LeaveCriticalSection(&sc->lock);
			return 0;
		}
		index = sc->pending[sc->first];
		LeaveCriticalSection(&sc->lock);

		/* The compositor samples the image itself: rendering is
		 * finished before it gets it. */
		t0 = now_ms();
		r = d->WaitForFences(d->handle, 1, &sc->fences[index], VK_TRUE, UINT64_MAX);
		d->ResetFences(d->handle, 1, &sc->fences[index]);
		t1 = now_ms();
		if (r == VK_SUCCESS)
			r = present_one(sc, index);
		t2 = now_ms();
		if (debug && (t1 - t0 > SLOW_MS || t2 - t1 > SLOW_MS))
			LOG("slow present: GPU %.1f ms, X %.1f ms\n", t1 - t0, t2 - t1);

		EnterCriticalSection(&sc->lock);
		sc->first = (sc->first + 1) % MAX_IMAGES;
		sc->queued--;
		sc->busy[index] = 0;
		if (r < 0)
			sc->status = r;
		WakeAllConditionVariable(&sc->cond);
		LeaveCriticalSection(&sc->lock);
	}
}

/* Queues each image behind the application's rendering and returns: the
 * swapchain's thread presents it once the GPU is done. */
static VkResult VKAPI_CALL dxgvk_QueuePresentKHR(VkQueue queue,
						 const VkPresentInfoKHR *info)
{
	VkPipelineStageFlags stages[16];
	VkResult r, overall = VK_SUCCESS;
	uint32_t i;

	for (i = 0; i < 16; i++)
		stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	for (i = 0; i < info->swapchainCount; i++) {
		struct swapchain *sc = (struct swapchain *)(uintptr_t)info->pSwapchains[i];
		uint32_t index = info->pImageIndices[i];
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

		if (debug) {
			double t = now_ms();

			if (sc->last_present && t - sc->last_present > SLOW_MS)
				LOG("slow frame: %.1f ms between presents\n",
				    t - sc->last_present);
			sc->last_present = t;
		}
		EnterCriticalSection(&sc->lock);
		r = sc->status;
		LeaveCriticalSection(&sc->lock);
		if (r >= 0) {
			/* The first submission waits for the application's
			 * semaphores; each signals its image's fence. */
			if (i == 0) {
				si.waitSemaphoreCount = info->waitSemaphoreCount < 16 ?
							info->waitSemaphoreCount : 16;
				si.pWaitSemaphores = info->pWaitSemaphores;
				si.pWaitDstStageMask = stages;
			}
			if (sc->hud && !hud_record(sc, queue_family(sc->dev, queue))) {
				hud_frame(sc->hud);
				si.commandBufferCount = 1;
				si.pCommandBuffers = &sc->hud->cmds[index];
			}
			r = sc->dev->QueueSubmit(queue, 1, &si, sc->fences[index]);
		}
		if (r == VK_SUCCESS) {
			EnterCriticalSection(&sc->lock);
			sc->pending[(sc->first + sc->queued) % MAX_IMAGES] = index;
			sc->queued++;
			sc->busy[index] = 1;
			WakeAllConditionVariable(&sc->cond);
			LeaveCriticalSection(&sc->lock);
			if (resized(sc))
				r = VK_SUBOPTIMAL_KHR;
		}
		if (info->pResults)
			info->pResults[i] = r;
		if (r < 0 || (r == VK_SUBOPTIMAL_KHR && overall == VK_SUCCESS))
			overall = r;
	}
	return overall;
}

/* --- Entry points -------------------------------------------------------- */

static const struct {
	const char *name;
	void *f;
} intercepted[] = {
#define I(name) { "vk" #name, (void *)dxgvk_##name }
	I(CreateInstance), I(EnumerateInstanceExtensionProperties),
	I(EnumeratePhysicalDevices), I(EnumerateDeviceExtensionProperties),
	I(CreateDevice), I(DestroyDevice), I(GetDeviceQueue), I(GetDeviceQueue2),
	I(CreateWin32SurfaceKHR), I(DestroySurfaceKHR),
	I(GetPhysicalDeviceSurfaceSupportKHR),
	I(GetPhysicalDeviceWin32PresentationSupportKHR),
	I(GetPhysicalDeviceSurfaceCapabilitiesKHR),
	I(GetPhysicalDeviceSurfaceCapabilities2KHR),
	I(GetPhysicalDeviceSurfaceFormatsKHR),
	I(GetPhysicalDeviceSurfaceFormats2KHR),
	I(GetPhysicalDeviceSurfacePresentModesKHR),
	I(GetPhysicalDevicePresentRectanglesKHR),
	I(GetDeviceGroupSurfacePresentModesKHR),
	I(GetDeviceGroupPresentCapabilitiesKHR),
	I(CreateSwapchainKHR), I(DestroySwapchainKHR),
	I(GetSwapchainImagesKHR), I(AcquireNextImageKHR),
	I(AcquireNextImage2KHR), I(QueuePresentKHR),
};

static void *intercept(const char *name)
{
	size_t i;

	if (!name)
		return NULL;
	for (i = 0; i < sizeof(intercepted) / sizeof(*intercepted); i++)
		if (!strcmp(name, intercepted[i].name))
			return intercepted[i].f;
	return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL dxgvk_GetDeviceProcAddr(VkDevice device,
							     const char *name);

static PFN_vkVoidFunction VKAPI_CALL dxgvk_GetInstanceProcAddr(VkInstance instance,
							       const char *name)
{
	void *f;

	if (ensure_init())
		return NULL;
	if (name && !strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)dxgvk_GetInstanceProcAddr;
	if (name && !strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)dxgvk_GetDeviceProcAddr;
	f = intercept(name);
	if (f)
		return (PFN_vkVoidFunction)f;
	return nv_gipa(instance, name);
}

static PFN_vkVoidFunction VKAPI_CALL dxgvk_GetDeviceProcAddr(VkDevice device,
							     const char *name)
{
	void *f;

	if (name && !strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)dxgvk_GetDeviceProcAddr;
	f = intercept(name);
	if (f)
		return (PFN_vkVoidFunction)f;
	return nv_gdpa(device, name);
}

EXPORT VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
	if (ensure_init())
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	if (*version > 7)
		*version = 7;
	return VK_SUCCESS;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
							       const char *name)
{
	return dxgvk_GetInstanceProcAddr(instance, name);
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
								     const char *name)
{
	void *f;

	if (ensure_init())
		return NULL;
	f = intercept(name);
	if (f)
		return (PFN_vkVoidFunction)f;
	return nv_gpdpa ? nv_gpdpa(instance, name) : NULL;
}
