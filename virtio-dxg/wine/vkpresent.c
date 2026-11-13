// SPDX-License-Identifier: MIT
/*
 * vkpresent: a Windows Vulkan window for testing presentation (dxgvk.dll
 * under Wine, or any Windows driver): clears the swapchain image to a
 * changing colour every frame and reports frames per second.
 *
 *   vkpresent.exe [seconds] [fifo|mailbox|immediate]
 */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>

#define CHECK(call)							\
	do {								\
		VkResult r_ = (call);					\
		if (r_ < 0) {						\
			printf("FAIL: %s: VkResult %d\n", #call, r_);	\
			exit(1);					\
		}							\
	} while (0)

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
	if (m == WM_DESTROY)
		PostQuitMessage(0);
	return DefWindowProcA(w, m, a, b);
}

static VkDevice device;
static VkSwapchainKHR swapchain;
static VkImage images[16];
static uint32_t image_count;
static VkExtent2D extent;

static void create_swapchain(VkPhysicalDevice gpu, VkSurfaceKHR surface,
			     VkPresentModeKHR mode)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSwapchainKHR old = swapchain;

	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
	extent = caps.currentExtent;
	ci.surface = surface;
	ci.minImageCount = caps.minImageCount + 1;
	ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
	ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	ci.imageExtent = extent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	ci.presentMode = mode;
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = old;
	CHECK(vkCreateSwapchainKHR(device, &ci, NULL, &swapchain));
	if (old) {
		vkDeviceWaitIdle(device);
		vkDestroySwapchainKHR(device, old, NULL);
	}
	image_count = 16;
	CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));
	printf("swapchain %ux%u, %u images\n", extent.width, extent.height, image_count);
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 8;
	VkPresentModeKHR mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkPhysicalDeviceProperties props;
	uint32_t n = 1;
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkQueue queue;
	VkSurfaceKHR surface;
	VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	VkCommandPool pool;
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandBuffer cmd[2];
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkSemaphore acquired[2], rendered[2];
	VkFence done[2];
	VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
	LARGE_INTEGER freq, t0, t;
	unsigned frames = 0, total = 0, recreated = 0;
	double last = 0;
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 2 && !strcmp(argv[2], "fifo"))
		mode = VK_PRESENT_MODE_FIFO_KHR;
	if (argc > 2 && !strcmp(argv[2], "mailbox"))
		mode = VK_PRESENT_MODE_MAILBOX_KHR;

	WNDCLASSA wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleA(NULL),
			 .lpszClassName = "vkpresent" };
	RegisterClassA(&wc);
	HWND win = CreateWindowA("vkpresent", "vkpresent", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
				 100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);

	app.pApplicationName = "vkpresent";
	app.apiVersion = VK_API_VERSION_1_3;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = iext;
	CHECK(vkCreateInstance(&ici, NULL, &instance));
	CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
	vkGetPhysicalDeviceProperties(gpu, &props);
	printf("GPU: %s, Vulkan %u.%u.%u\n", props.deviceName,
	       VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	       VK_API_VERSION_PATCH(props.apiVersion));
	sci.hinstance = wc.hInstance;
	sci.hwnd = win;
	CHECK(vkCreateWin32SurfaceKHR(instance, &sci, NULL, &surface));
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = dext;
	CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
	vkGetDeviceQueue(device, 0, 0, &queue);
	create_swapchain(gpu, surface, mode);

	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
	cai.commandPool = pool;
	cai.commandBufferCount = 2;
	CHECK(vkAllocateCommandBuffers(device, &cai, cmd));
	for (i = 0; i < 2; i++) {
		CHECK(vkCreateSemaphore(device, &semi, NULL, &acquired[i]));
		CHECK(vkCreateSemaphore(device, &semi, NULL, &rendered[i]));
		CHECK(vkCreateFence(device, &fci, NULL, &done[i]));
	}

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (int f = 0;; f++) {
		MSG msg;
		int k = f & 1;
		uint32_t index;
		VkResult r;
		double now;

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				return 0;
			DispatchMessageA(&msg);
		}
		QueryPerformanceCounter(&t);
		now = (double)(t.QuadPart - t0.QuadPart) / freq.QuadPart;
		if (now >= seconds)
			break;

		CHECK(vkWaitForFences(device, 1, &done[k], VK_TRUE, UINT64_MAX));
		r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[k],
					  VK_NULL_HANDLE, &index);
		if (r == VK_ERROR_OUT_OF_DATE_KHR) {
			create_swapchain(gpu, surface, mode);
			recreated++;
			continue;
		}
		CHECK(r);
		CHECK(vkResetFences(device, 1, &done[k]));

		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		VkClearColorValue color = { { 0.5f + 0.5f * (float)sin(now * 3),
					      0.5f + 0.5f * (float)sin(now * 2 + 2),
					      0.5f + 0.5f * (float)sin(now + 4), 1 } };

		CHECK(vkResetCommandBuffer(cmd[k], 0));
		CHECK(vkBeginCommandBuffer(cmd[k], &bi));
		b.srcAccessMask = 0;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = images[index];
		b.subresourceRange = range;
		vkCmdPipelineBarrier(cmd[k], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
		vkCmdClearColorImage(cmd[k], images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				     &color, 1, &range);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cmd[k], VK_PIPELINE_STAGE_TRANSFER_BIT,
				     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
		CHECK(vkEndCommandBuffer(cmd[k]));

		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired[k],
				    &wait_stage, 1, &cmd[k], 1, &rendered[k] };
		CHECK(vkQueueSubmit(queue, 1, &si, done[k]));

		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1,
					&rendered[k], 1, &swapchain, &index, NULL };
		r = vkQueuePresentKHR(queue, &pi);
		if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
			vkDeviceWaitIdle(device);
			create_swapchain(gpu, surface, mode);
			recreated++;
		} else {
			CHECK(r);
		}
		frames++;
		total++;
		if (now - last >= 1) {
			printf("%u fps\n", frames);
			frames = 0;
			last = now;
		}
	}
	vkDeviceWaitIdle(device);
	printf("PASS: %u frames in %.1f s (%.0f fps average), %u swapchain recreations\n",
	       total, seconds, total / seconds, recreated);
	return 0;
}
