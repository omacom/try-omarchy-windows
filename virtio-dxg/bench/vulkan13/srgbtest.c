/* srgbtest: sRGB conversions on Dozen. vkCmdBlitImage converts through
 * linear (an sRGB source is decoded, an sRGB destination encoded); copies
 * move raw bytes. Every case starts from raw byte 128 in the source.
 *
 *   gcc -O2 -o srgbtest srgbtest.c -lvulkan && ./srgbtest
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static VkBuffer buf;
static uint8_t *bufp;
static int failures;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags f)
{
	VkPhysicalDeviceMemoryProperties mp;

	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f)
			return i;
	exit(1);
}

static VkImage image(VkFormat fmt, int size, VkImageCreateFlags flags, const VkImageFormatListCreateInfo *list,
		     VkImageUsageFlags extra)
{
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, list, .flags = flags,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { size, size, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra };
	VkMemoryRequirements req;
	VkDeviceMemory mem;
	VkImage img;

	CHECK(vkCreateImage(dev, &ii, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	return img;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
			     0, NULL, 1, &b);
}

/* raw 128 into src (size s), blit to dst (size d), raw bytes back */
static int blit(VkFormat sf, VkFormat df, int s, int d, VkFilter filter, VkImageCreateFlags flags,
		VkImageUsageFlags extra)
{
	VkFormat both[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
	VkImageFormatListCreateInfo list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2, .pViewFormats = both };
	VkImage src = image(sf, s, flags, flags ? &list : NULL, extra);
	VkImage dst = image(df, d, flags, flags ? &list : NULL, extra);
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

	memset(bufp, 128, 64 * 64 * 4);
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));
	barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { s, s, 1 } };
	vkCmdCopyBufferToImage(cmd, buf, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
	barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkImageBlit region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { s, s, 1 } },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { d, d, 1 } } };
	vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		       1, &region, filter);
	barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy down = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { d, d, 1 } };
	VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buf, .size = VK_WHOLE_SIZE };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
			     1, &hb, 0, NULL);
	vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &down);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, cpool, 1, &cmd);
	return bufp[(d / 2 * d + d / 2) * 4];
}

static void check(const char *name, int got, int want)
{
	int ok = abs(got - want) <= 2;

	printf("%s %-46s %3d (want %d)\n", ok ? "ok  " : "FAIL", name, got, want);
	failures += !ok;
}

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	uint32_t n = 1;
	float prio = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &cpool));
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 * 64 * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkMemoryRequirements req;
	VkDeviceMemory mem;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
							   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&bufp));

	const VkFormat U = VK_FORMAT_R8G8B8A8_UNORM, S = VK_FORMAT_R8G8B8A8_SRGB;
	for (int mutable = 0; mutable < 2; mutable++) {
		VkImageCreateFlags fl = mutable ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
		const char *m = mutable ? " (mutable)" : "";
		char name[96];

#define CASE(label, sf, df, s, d, f, want) \
		snprintf(name, sizeof(name), "%s%s", label, m); check(name, blit(sf, df, s, d, f, fl, 0), want)
		CASE("blit sRGB->UNORM", S, U, 16, 16, VK_FILTER_NEAREST, 55);
		CASE("blit UNORM->sRGB", U, S, 16, 16, VK_FILTER_NEAREST, 188);
		CASE("blit sRGB->sRGB", S, S, 16, 16, VK_FILTER_NEAREST, 128);
		CASE("blit UNORM->UNORM", U, U, 16, 16, VK_FILTER_NEAREST, 128);
		CASE("blit sRGB->UNORM scaled, linear", S, U, 32, 16, VK_FILTER_LINEAR, 55);
		CASE("blit UNORM->sRGB scaled, linear", U, S, 32, 16, VK_FILTER_LINEAR, 188);
	}

	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf(failures ? "FAILED: %d\n" : "PASS: sRGB blits\n", failures);
	return failures != 0;
}
