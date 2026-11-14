// SPDX-License-Identifier: MIT
/*
 * nvvkprobe: runs NVIDIA's Windows Vulkan driver inside Linux.
 *
 * Under Wine, loads the vendor ICD from the host driver store (mounted in
 * the guest), routes its D3DKMT calls to /dev/dxg through kmt_bridge.c, and
 * exercises it: instance, physical device, feature report, then a compute
 * dispatch whose every output is checked on the CPU.
 *
 *   wine nvvkprobe.exe [--trace] [path\to\nvoglv64.dll]
 */
#include <windows.h>
#include <winternl.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "kmt_bridge.h"
#include "probe_compute_spv.h"

#include "nvload.h"

static int trace;
/* --native: the same test on Windows itself, as the baseline. */
static int native;
static LUID driver_luid;

/* --- Vulkan ------------------------------------------------------------ */

#define VKCHECK(call)							\
	do {								\
		VkResult r_ = (call);					\
		if (r_ != VK_SUCCESS) {					\
			printf("FAILED %s: VkResult %d\n", #call, r_);	\
			return 1;					\
		}							\
	} while (0)

static double now_ms(void)
{
	LARGE_INTEGER t, f;

	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint32_t expected(uint32_t i)
{
	uint32_t x = i * 2654435761u;
	int k;

	for (k = 0; k < 64; k++)
		x = x * 1664525u + 1013904223u;
	return x;
}

static int run_vulkan(HMODULE icd)
{
	PFN_vkGetInstanceProcAddr gipa =
		(PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
	VkResult(VKAPI_PTR *negotiate)(uint32_t *) =
		(void *)GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
	uint32_t version = 7, count = 0, i, family = UINT32_MAX;
	const uint32_t n = 1u << 22;	/* 4M invocations, 16 MiB */
	VkInstance instance;
	VkPhysicalDevice gpus[8];
	VkDevice device;
	VkQueue queue;
	double t0;

	if (!gipa || !negotiate) {
		printf("FAILED: the DLL exports no Vulkan ICD interface\n");
		return 1;
	}
	VKCHECK(negotiate(&version));
	printf("ICD interface version %u\n", version);

	PFN_vkCreateInstance vkCreateInstance =
		(PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

	app.pApplicationName = "nvvkprobe";
	app.apiVersion = VK_API_VERSION_1_3;
	ici.pApplicationInfo = &app;
	t0 = now_ms();
	VKCHECK(vkCreateInstance(&ici, NULL, &instance));
	printf("vkCreateInstance: %.1f ms\n", now_ms() - t0);

#define IPROC(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
	IPROC(vkEnumeratePhysicalDevices);
	IPROC(vkGetPhysicalDeviceProperties2);
	IPROC(vkGetPhysicalDeviceFeatures2);
	IPROC(vkEnumerateDeviceExtensionProperties);
	IPROC(vkGetPhysicalDeviceQueueFamilyProperties);
	IPROC(vkGetPhysicalDeviceMemoryProperties);
	IPROC(vkCreateDevice);
	IPROC(vkGetDeviceProcAddr);
	IPROC(vkDestroyInstance);

	/*
	 * On Windows the loader enumerates adapters and passes each LUID to
	 * the ICD (interface version 6+); the probe plays the loader.
	 */
	{
		VkResult(VKAPI_PTR *by_adapter)(VkInstance, LUID, uint32_t *,
						VkPhysicalDevice *) =
			(void *)GetProcAddress(icd,
				"vk_icdEnumerateAdapterPhysicalDevices");

		count = 8;
		t0 = now_ms();
		if (by_adapter)
			VKCHECK(by_adapter(instance, driver_luid, &count, gpus));
		else
			VKCHECK(vkEnumeratePhysicalDevices(instance, &count, gpus));
		printf("%s: %u device(s), %.1f ms\n", by_adapter ?
		       "vk_icdEnumerateAdapterPhysicalDevices" :
		       "vkEnumeratePhysicalDevices", count, now_ms() - t0);
	}
	if (!count)
		return 1;

	VkPhysicalDeviceDriverProperties driver = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 props = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
	vkGetPhysicalDeviceProperties2(gpus[0], &props);
	printf("GPU: %s, Vulkan %u.%u.%u, driver %s %s\n",
	       props.properties.deviceName,
	       VK_API_VERSION_MAJOR(props.properties.apiVersion),
	       VK_API_VERSION_MINOR(props.properties.apiVersion),
	       VK_API_VERSION_PATCH(props.properties.apiVersion),
	       driver.driverName, driver.driverInfo);

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rt };
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, &as };
	VkPhysicalDeviceFeatures2 features = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &mesh };
	vkGetPhysicalDeviceFeatures2(gpus[0], &features);
	printf("Ray tracing pipeline: %s, acceleration structures: %s, "
	       "mesh shaders: %s\n", rt.rayTracingPipeline ? "yes" : "no",
	       as.accelerationStructure ? "yes" : "no",
	       mesh.meshShader ? "yes" : "no");

	{
		static const char *wanted[] = {
			"VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_query",
			"VK_NVX_binary_import", "VK_NVX_image_view_handle",
			"VK_NV_low_latency2", "VK_NV_optical_flow",
			"VK_KHR_video_decode_queue", "VK_KHR_video_encode_queue",
			"VK_KHR_external_memory_win32", "VK_KHR_swapchain",
		};
		VkExtensionProperties *ext;
		uint32_t n_ext = 0, w;

		vkEnumerateDeviceExtensionProperties(gpus[0], NULL, &n_ext, NULL);
		ext = calloc(n_ext, sizeof(*ext));
		vkEnumerateDeviceExtensionProperties(gpus[0], NULL, &n_ext, ext);
		printf("%u device extensions; key ones:", n_ext);
		for (w = 0; w < sizeof(wanted) / sizeof(*wanted); w++) {
			int found = 0;

			for (i = 0; i < n_ext; i++)
				found |= !strcmp(ext[i].extensionName, wanted[w]);
			printf(" %s%s", found ? "+" : "-", wanted[w] + 3);
		}
		printf("\n");
		free(ext);
	}

	{
		VkQueueFamilyProperties qf[16];
		uint32_t nqf = 16;

		vkGetPhysicalDeviceQueueFamilyProperties(gpus[0], &nqf, qf);
		for (i = 0; i < nqf && family == UINT32_MAX; i++)
			if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
				family = i;
	}
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	t0 = now_ms();
	VKCHECK(vkCreateDevice(gpus[0], &dci, NULL, &device));
	printf("vkCreateDevice: %.1f ms\n", now_ms() - t0);

#define DPROC(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
	DPROC(vkGetDeviceQueue);
	DPROC(vkCreateBuffer);
	DPROC(vkGetBufferMemoryRequirements);
	DPROC(vkAllocateMemory);
	DPROC(vkBindBufferMemory);
	DPROC(vkMapMemory);
	DPROC(vkCreateShaderModule);
	DPROC(vkCreateDescriptorSetLayout);
	DPROC(vkCreatePipelineLayout);
	DPROC(vkCreateComputePipelines);
	DPROC(vkCreateDescriptorPool);
	DPROC(vkAllocateDescriptorSets);
	DPROC(vkUpdateDescriptorSets);
	DPROC(vkCreateCommandPool);
	DPROC(vkAllocateCommandBuffers);
	DPROC(vkBeginCommandBuffer);
	DPROC(vkCmdBindPipeline);
	DPROC(vkCmdBindDescriptorSets);
	DPROC(vkCmdDispatch);
	DPROC(vkEndCommandBuffer);
	DPROC(vkCreateFence);
	DPROC(vkResetFences);
	DPROC(vkQueueSubmit);
	DPROC(vkWaitForFences);
	DPROC(vkDeviceWaitIdle);
	DPROC(vkDestroyDevice);
	DPROC(vkCreateQueryPool);
	DPROC(vkCmdResetQueryPool);
	DPROC(vkCmdWriteTimestamp);
	DPROC(vkGetQueryPoolResults);
	vkGetDeviceQueue(device, family, 0, &queue);

	/* A host-visible, preferably cached buffer the shader fills. */
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkBuffer buffer;
	VkMemoryRequirements req;
	VkPhysicalDeviceMemoryProperties mp;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkDeviceMemory memory;
	uint32_t *data;
	int best = -1;

	bci.size = (VkDeviceSize)n * 4;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VKCHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
	vkGetBufferMemoryRequirements(device, buffer, &req);
	vkGetPhysicalDeviceMemoryProperties(gpus[0], &mp);
	for (i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;

		if (!(req.memoryTypeBits & (1u << i)) ||
		    !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
		    !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			continue;
		if (best < 0 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
			best = (int)i;
	}
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = (uint32_t)best;
	VKCHECK(vkAllocateMemory(device, &mai, NULL, &memory));
	VKCHECK(vkBindBufferMemory(device, buffer, memory, 0));
	VKCHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data));
	memset(data, 0, (size_t)n * 4);

	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule shader;
	smci.codeSize = sizeof(probe_compute_spv);
	smci.pCode = probe_compute_spv;
	VKCHECK(vkCreateShaderModule(device, &smci, NULL, &shader));

	VkDescriptorSetLayoutBinding binding = { 0,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayout dsl;
	dslci.bindingCount = 1;
	dslci.pBindings = &binding;
	VKCHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));

	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	VKCHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));

	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkPipeline pipeline;
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = shader;
	cpci.stage.pName = "main";
	cpci.layout = layout;
	t0 = now_ms();
	VKCHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
					 &pipeline));
	printf("compute pipeline (shader compiled in the guest): %.1f ms\n",
	       now_ms() - t0);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorPool pool;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VKCHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkDescriptorSet set;
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &dsl;
	VKCHECK(vkAllocateDescriptorSets(device, &dsai, &set));
	VkDescriptorBufferInfo dbi = { buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	wds.dstSet = set;
	wds.descriptorCount = 1;
	wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	wds.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(device, 1, &wds, 0, NULL);

	VkCommandPoolCreateInfo cpoolci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool cpool;
	cpoolci.queueFamilyIndex = family;
	VKCHECK(vkCreateCommandPool(device, &cpoolci, NULL, &cpool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBuffer cb;
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VKCHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
	/* Timestamps around the dispatch give the GPU's own execution time. */
	VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	VkQueryPool timestamps;
	qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qpci.queryCount = 2;
	VKCHECK(vkCreateQueryPool(device, &qpci, NULL, &timestamps));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	VKCHECK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdResetQueryPool(cb, timestamps, 0, 2);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
				&set, 0, NULL);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
	vkCmdDispatch(cb, n / 64, 1, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
	VKCHECK(vkEndCommandBuffer(cb));

	/* An almost empty command buffer: what one submission costs. */
	VkCommandBuffer tiny;
	VKCHECK(vkAllocateCommandBuffers(device, &cbai, &tiny));
	VKCHECK(vkBeginCommandBuffer(tiny, &cbbi));
	vkCmdBindPipeline(tiny, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(tiny, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
				1, &set, 0, NULL);
	vkCmdDispatch(tiny, 1, 1, 1);
	VKCHECK(vkEndCommandBuffer(tiny));

	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	double best_ms = 1e9;
	int round;

	VKCHECK(vkCreateFence(device, &fci, NULL, &fence));
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	for (round = 0; round < 20; round++) {
		double t;

		VKCHECK(vkResetFences(device, 1, &fence));
		t = now_ms();
		VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		t = now_ms() - t;
		if (t < best_ms)
			best_ms = t;
	}
	printf("dispatch of %u invocations x 64 iterations: best %.3f ms "
	       "submit-to-signal over 20 rounds\n", n, best_ms);

	{
		uint64_t ts[2];
		double gpu_ms, total;
		const int batch = 200, submits = 2000;

		VKCHECK(vkGetQueryPoolResults(device, timestamps, 0, 2,
					      sizeof(ts), ts, sizeof(ts[0]),
					      VK_QUERY_RESULT_64_BIT |
					      VK_QUERY_RESULT_WAIT_BIT));
		gpu_ms = (double)(ts[1] - ts[0]) *
			 props.properties.limits.timestampPeriod / 1e6;
		printf("GPU execution time of that dispatch: %.3f ms\n", gpu_ms);

		/* Back to back, as frames are: submission overlaps the GPU. */
		VKCHECK(vkResetFences(device, 1, &fence));
		total = now_ms();
		for (round = 0; round < batch; round++)
			VKCHECK(vkQueueSubmit(queue, 1, &si,
					      round == batch - 1 ? fence :
					      VK_NULL_HANDLE));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		total = now_ms() - total;
		printf("pipelined: %d dispatches in %.1f ms, %.3f ms each "
		       "(GPU-bound: %.0f%% of GPU time)\n", batch, total,
		       total / batch, 100.0 * gpu_ms * batch / total);

		si.pCommandBuffers = &tiny;
		VKCHECK(vkResetFences(device, 1, &fence));
		total = now_ms();
		for (round = 0; round < submits; round++)
			VKCHECK(vkQueueSubmit(queue, 1, &si,
					      round == submits - 1 ? fence :
					      VK_NULL_HANDLE));
		VKCHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		total = now_ms() - total;
		printf("submission rate: %d submits in %.1f ms, %.1f us each, "
		       "%.0f per second\n", submits, total,
		       total * 1000.0 / submits, submits * 1000.0 / total);
		si.pCommandBuffers = &cb;
	}

	{
		uint32_t bad = 0;

		for (i = 0; i < n; i++)
			bad += data[i] != expected(i);
		printf("compute result: %u of %u values correct%s\n", n - bad, n,
		       bad ? "  <-- MISMATCH" : "");
		vkDeviceWaitIdle(device);
		vkDestroyDevice(device, NULL);
		vkDestroyInstance(instance, NULL);
		return bad ? 1 : 0;
	}
}

int main(int argc, char **argv)
{
	wchar_t path[MAX_PATH] = L"";
	HMODULE icd = NULL;
	int i, ret;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trace"))
			trace = 1;
		else if (!strcmp(argv[i], "--stacks"))
			trace = 2;
		else if (!strcmp(argv[i], "--native"))
			native = 1;
	}
	if (native) {
		if (nv_find_driver(path, &driver_luid, 1)) {
			printf("FAILED: no NVIDIA ICD reported by the adapter\n");
			return 1;
		}
		printf("loading %ls\n", path);
		icd = LoadLibraryW(path);
	} else {
		wchar_t so[MAX_PATH];

		GetModuleFileNameW(NULL, so, MAX_PATH);
		wcscpy(wcsrchr(so, L'\\') + 1, L"dxgkmt.so");
		if (nv_load(trace, so, &icd, &driver_luid))
			icd = NULL;
	}
	if (!icd) {
		printf("FAILED: the driver did not load\n");
		return 1;
	}
	ret = run_vulkan(icd);
	if (!native)
		kmt_bridge_report();
	printf("%s\n", ret ? "PROBE FAILED" : "PROBE PASSED");
	return ret;
}
