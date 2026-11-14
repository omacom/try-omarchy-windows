/* samptest: sampling sRGB images on Dozen must decode. A 4x4 image holding
 * raw 188 (sRGB for 0.502) is sampled into a UNORM target: expect 128.
 * Variants: plain / mutable with a format list / plus storage usage, and
 * Dozen's descriptor-heap and bindless modes (BINDLESS=1 enables buffer
 * device addresses, which puts Dozen in bindless mode).
 *
 *   glslc samp.vert -o samp.vert.spv; glslc samp.frag -o samp.frag.spv
 *   gcc -O2 -o samptest samptest.c -lvulkan && ./samptest
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

static VkImage image(VkFormat fmt, int size, VkImageCreateFlags flags, const void *next, VkImageUsageFlags usage)
{
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, next, .flags = flags,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { size, size, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage };
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

static VkImageView view(VkImage img, VkFormat fmt)
{
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView v;

	CHECK(vkCreateImageView(dev, &vi, NULL, &v));
	return v;
}

static VkShaderModule shader(const char *path)
{
	static uint32_t code[8192];
	FILE *f = fopen(path, "rb");
	size_t n;
	VkShaderModule m;

	if (!f)
		exit(1);
	n = fread(code, 1, sizeof(code), f);
	fclose(f);
	VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
	CHECK(vkCreateShaderModule(dev, &si, NULL, &m));
	return m;
}

static void ibar(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
			     0, NULL, 1, &b);
}

static VkDescriptorSetLayout dsl;
static VkPipelineLayout pl;
static VkPipeline pipe;
static VkSampler smp;
static VkDescriptorPool dpool;
static VkPipeline cpipe;

/* renders constant 0.5 into image (format f, flags) through view format vf;
 * raw byte back: 188 for an sRGB view */
static int render(VkFormat f, VkFormat vf, VkImageCreateFlags flags, VkImageUsageFlags extra)
{
	VkFormat both[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
	VkImageFormatListCreateInfo list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2, .pViewFormats = both };
	VkImage dst = image(f, 4, flags, flags ? &list : NULL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra);
	VkImageView dv = view(dst, vf);
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkBufferImageCopy down = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 4, 4, 1 } };

	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));
	ibar(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 4, 4 } },
		.layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
	vkCmdBeginRendering(cmd, &ri);
	VkViewport vp = { 0, 0, 4, 4, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { 4, 4 } };
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	ibar(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &down);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, cpool, 1, &cmd);
	return bufp[(2 * 4 + 2) * 4];
}

/* source: format sf, view format vf; raw bytes 188 uploaded */
static int sample(VkFormat sf, VkFormat vf, VkImageCreateFlags flags, VkImageUsageFlags extra)
{
	VkFormat both[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
	VkImageFormatListCreateInfo list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2, .pViewFormats = both };
	VkImage src = image(sf, 4, flags, flags ? &list : NULL, VK_IMAGE_USAGE_SAMPLED_BIT |
			    VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra);
	VkImage dst = image(VK_FORMAT_R8G8B8A8_UNORM, 4, 0, NULL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			    VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	VkImageView sv = view(src, vf), dv = view(dst, VK_FORMAT_R8G8B8A8_UNORM);
	VkDescriptorSet set;
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorImageInfo dii = { smp, sv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	memset(bufp, 188, 4096);
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));
	ibar(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 4, 4, 1 } };
	vkCmdCopyBufferToImage(cmd, buf, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
	ibar(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	ibar(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 4, 4 } },
		.layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
	vkCmdBeginRendering(cmd, &ri);
	VkViewport vp = { 0, 0, 4, 4, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { 4, 4 } };
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	ibar(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &up);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, cpool, 1, &cmd);
	return bufp[(2 * 4 + 2) * 4];
}

static void check(const char *name, int got, int want)
{
	int ok = abs(got - want) <= 2;

	printf("%s %-52s %3d (want %d)\n", ok ? "ok  " : "FAIL", name, got, want);
	failures += !ok;
}

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	uint32_t n = 1;
	float prio = 1;
	int bindless = getenv("BINDLESS") != NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.dynamicRendering = VK_TRUE };
	VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13,
		.bufferDeviceAddress = bindless };
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &cpool));
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
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

	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST, .maxLod = 1 };
	CHECK(vkCreateSampler(dev, &sci, NULL, &smp));
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT };
	VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b };
	CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));

	VkPipelineShaderStageCreateInfo st[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = shader("samp.vert.spv"), .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = shader("samp.frag.spv"), .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .scissorCount = 1 };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dyn };
	VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
	VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cbs, .pDynamicState = &dys, .layout = pl };
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
	{
		VkFormat sf = VK_FORMAT_R8G8B8A8_SRGB;
		VkPipelineRenderingCreateInfo pri2 = pri;
		VkPipelineShaderStageCreateInfo st2[2] = { st[0], st[1] };
		VkGraphicsPipelineCreateInfo g2 = gpi;

		pri2.pColorAttachmentFormats = &sf;
		st2[1].module = shader("half.frag.spv");
		g2.pNext = &pri2;
		g2.pStages = st2;
		CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g2, NULL, &cpipe));
	}

	const VkFormat U = VK_FORMAT_R8G8B8A8_UNORM, S = VK_FORMAT_R8G8B8A8_SRGB;
	const VkImageCreateFlags M = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	printf("descriptor mode: %s\n", bindless ? "bindless" : "descriptor heaps");
	check("sRGB image, sRGB view", sample(S, S, 0, 0), 128);
	check("sRGB image (mutable), sRGB view", sample(S, S, M, 0), 128);
	check("sRGB image (mutable), UNORM view", sample(S, U, M, 0), 188);
	check("UNORM image (mutable), sRGB view", sample(U, S, M, 0), 128);
	check("UNORM image (mutable, storage), sRGB view", sample(U, S, M, VK_IMAGE_USAGE_STORAGE_BIT), 128);
	check("sRGB image (mutable, color att.), sRGB view", sample(S, S, M, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), 128);

	check("render 0.5 into sRGB image", render(S, S, 0, 0), 188);
	check("render 0.5 into sRGB view of mutable sRGB image", render(S, S, M, 0), 188);
	check("render 0.5 into sRGB view of mutable UNORM image", render(U, S, M, 0), 188);
	check("render 0.5 into sRGB view, UNORM image + storage", render(U, S, M, VK_IMAGE_USAGE_STORAGE_BIT), 188);
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf(failures ? "FAILED: %d\n" : "PASS: sRGB sampling\n", failures);
	return failures != 0;
}
