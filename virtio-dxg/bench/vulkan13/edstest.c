/* edstest: Vulkan 1.3 extended dynamic state on Dozen, offscreen, checked by
 * reading pixels back. Each case renders into a 64x64 image after clearing
 * it to black / depth 1 / stencil 0.
 *
 *   glslc edstest.vert -o vert.spv; glslc edstest.frag -o frag.spv
 *   gcc -O2 -o edstest edstest.c -lvulkan && ./edstest
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkImage color, depth;
static VkImageView color_view, depth_view;
static VkBuffer vb, ib, rb;
static VkDeviceMemory vb_mem, ib_mem, rb_mem;
static float *vb_ptr;
static uint8_t *rb_ptr;
static void *ib_ptr;
static VkPipeline pipe;
static VkPipelineLayout layout;
static int failures;

static PFN_vkCmdBeginRenderingKHR BeginRendering;
static PFN_vkCmdEndRenderingKHR EndRendering;
static PFN_vkCmdSetCullModeEXT SetCullMode;
static PFN_vkCmdSetFrontFaceEXT SetFrontFace;
static PFN_vkCmdSetPrimitiveTopologyEXT SetPrimitiveTopology;
static PFN_vkCmdSetViewportWithCountEXT SetViewportWithCount;
static PFN_vkCmdSetScissorWithCountEXT SetScissorWithCount;
static PFN_vkCmdBindVertexBuffers2EXT BindVertexBuffers2;
static PFN_vkCmdSetDepthTestEnableEXT SetDepthTestEnable;
static PFN_vkCmdSetDepthWriteEnableEXT SetDepthWriteEnable;
static PFN_vkCmdSetDepthCompareOpEXT SetDepthCompareOp;
static PFN_vkCmdSetStencilTestEnableEXT SetStencilTestEnable;
static PFN_vkCmdSetStencilOpEXT SetStencilOp;
static PFN_vkCmdSetRasterizerDiscardEnableEXT SetRasterizerDiscardEnable;
static PFN_vkCmdSetDepthBiasEnableEXT SetDepthBiasEnable;
static PFN_vkCmdSetPrimitiveRestartEnableEXT SetPrimitiveRestartEnable;

/* Vertex buffer layout (vec4 per vertex, stride 16 unless noted). */
enum {
	VB_QUAD02 = 0,		/* full quad, 2 triangles, z = 0.2 */
	VB_QUAD05 = 6,		/* z = 0.5 */
	VB_QUAD08 = 12,		/* z = 0.8 */
	VB_LEFT = 18,		/* left half, z = 0.5 */
	VB_STRIP = 24,		/* 4 vertices TL TR BL BR, z = 0.5 */
	VB_CORNERS = 28,	/* 6 vertices: top-left and bottom-right corner triangles */
	VB_WIDE = 40,		/* full quad at stride 32 (12 slots) */
	VB_COUNT = 64,
};

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties mp;

	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	printf("FAIL no memory type\n");
	exit(1);
}

static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
			VkDeviceMemory *mem, void **ptr)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkMemoryRequirements req;

	CHECK(vkCreateBuffer(dev, &bi, NULL, buf));
	vkGetBufferMemoryRequirements(dev, *buf, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
							   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, mem));
	CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
	CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, ptr));
}

static void make_image(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
		       VkImage *img, VkImageView *view)
{
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = fmt, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
	VkMemoryRequirements req;
	VkDeviceMemory mem;

	CHECK(vkCreateImage(dev, &ii, NULL, img));
	vkGetImageMemoryRequirements(dev, *img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, *img, mem, 0));
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { aspect, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vi, NULL, view));
}

static VkShaderModule load_shader(const char *path)
{
	FILE *f = fopen(path, "rb");
	static uint32_t code[16384];
	size_t n;
	VkShaderModule m;

	if (!f) {
		printf("FAIL open %s\n", path);
		exit(1);
	}
	n = fread(code, 1, sizeof(code), f);
	fclose(f);
	VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
	CHECK(vkCreateShaderModule(dev, &si, NULL, &m));
	return m;
}

static void put(int slot, float x, float y, float z)
{
	vb_ptr[slot * 4 + 0] = x;
	vb_ptr[slot * 4 + 1] = y;
	vb_ptr[slot * 4 + 2] = z;
	vb_ptr[slot * 4 + 3] = 1;
}

static void quad(int slot, float x0, float y0, float x1, float y1, float z)
{
	put(slot + 0, x0, y0, z);
	put(slot + 1, x1, y0, z);
	put(slot + 2, x0, y1, z);
	put(slot + 3, x1, y0, z);
	put(slot + 4, x1, y1, z);
	put(slot + 5, x0, y1, z);
}

static void init(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	uint32_t n = 1;
	VkPhysicalDeviceProperties props;

	CHECK(vkCreateInstance(&ici, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 ? VK_ERROR_UNKNOWN : VK_SUCCESS);
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
	       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
		.extendedDynamicState2 = VK_TRUE };
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, &eds2,
		.extendedDynamicState = VK_TRUE };
	VkPhysicalDeviceDynamicRenderingFeatures dr = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &eds, .dynamicRendering = VK_TRUE };
	const char *exts[] = { "VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state",
			       "VK_EXT_extended_dynamic_state2" };
	float prio = 1;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dr, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi, .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);

#define GET(var, name) do { var = (void *)vkGetDeviceProcAddr(dev, name); \
	if (!var) { printf("FAIL no %s\n", name); exit(1); } } while (0)
	GET(BeginRendering, "vkCmdBeginRenderingKHR");
	GET(EndRendering, "vkCmdEndRenderingKHR");
	GET(SetCullMode, "vkCmdSetCullModeEXT");
	GET(SetFrontFace, "vkCmdSetFrontFaceEXT");
	GET(SetPrimitiveTopology, "vkCmdSetPrimitiveTopologyEXT");
	GET(SetViewportWithCount, "vkCmdSetViewportWithCountEXT");
	GET(SetScissorWithCount, "vkCmdSetScissorWithCountEXT");
	GET(BindVertexBuffers2, "vkCmdBindVertexBuffers2EXT");
	GET(SetDepthTestEnable, "vkCmdSetDepthTestEnableEXT");
	GET(SetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT");
	GET(SetDepthCompareOp, "vkCmdSetDepthCompareOpEXT");
	GET(SetStencilTestEnable, "vkCmdSetStencilTestEnableEXT");
	GET(SetStencilOp, "vkCmdSetStencilOpEXT");
	GET(SetRasterizerDiscardEnable, "vkCmdSetRasterizerDiscardEnableEXT");
	GET(SetDepthBiasEnable, "vkCmdSetDepthBiasEnableEXT");
	GET(SetPrimitiveRestartEnable, "vkCmdSetPrimitiveRestartEnableEXT");

	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

	make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		   VK_IMAGE_ASPECT_COLOR_BIT, &color, &color_view);
	make_image(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &depth, &depth_view);
	make_buffer(VB_COUNT * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vb_mem, (void **)&vb_ptr);
	make_buffer(64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &ib, &ib_mem, &ib_ptr);
	make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rb_mem, (void **)&rb_ptr);

	quad(VB_QUAD02, -1, -1, 1, 1, 0.2f);
	quad(VB_QUAD05, -1, -1, 1, 1, 0.5f);
	quad(VB_QUAD08, -1, -1, 1, 1, 0.8f);
	quad(VB_LEFT, -1, -1, 0, 1, 0.5f);
	put(VB_STRIP + 0, -1, -1, 0.5f);
	put(VB_STRIP + 1, 1, -1, 0.5f);
	put(VB_STRIP + 2, -1, 1, 0.5f);
	put(VB_STRIP + 3, 1, 1, 0.5f);
	put(VB_CORNERS + 0, -1, -1, 0.5f);
	put(VB_CORNERS + 1, -0.5f, -1, 0.5f);
	put(VB_CORNERS + 2, -1, -0.5f, 0.5f);
	put(VB_CORNERS + 3, 1, 1, 0.5f);
	put(VB_CORNERS + 4, 0.5f, 1, 0.5f);
	put(VB_CORNERS + 5, 1, 0.5f, 0.5f);
	/* the stride-32 quad: every other vec4 */
	{
		float q[6][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };

		for (int i = 0; i < 6; i++) {
			put(VB_WIDE + 2 * i, q[i][0], q[i][1], 0.5f);
			put(VB_WIDE + 2 * i + 1, 9, 9, 9);	/* garbage at stride 16 */
		}
	}
	/* strip with a restart between two corner triangles */
	{
		uint16_t idx[7] = { 0, 1, 2, 0xffff, 3, 4, 5 };

		memcpy(ib_ptr, idx, sizeof(idx));
	}

	/* pipeline: everything extended-dynamic */
	VkShaderModule vs = load_shader("vert.spv"), fs = load_shader("frag.spv");
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = vs, .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = fs, .pName = "main" },
	};
	VkVertexInputBindingDescription bind = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
	VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
		.vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr };
	VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_CLOCKWISE, .depthBiasConstantFactor = 100000, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.front = { .compareMask = 0xff, .writeMask = 0xff },
		.back = { .compareMask = 0xff, .writeMask = 0xff } };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[] = {
		VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
		VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
		VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
		VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
		VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
		VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
		VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
	};
	VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]), .pDynamicStates = dyn };
	VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt,
		.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
	VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri,
		.stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dys, .layout = layout };
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
}

static VkCommandBuffer begin(void)
{
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));

	VkImageMemoryBarrier b[2] = {
		{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		  .image = color, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
		{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		  .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		  .image = depth,
		  .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 } },
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			     0, 0, NULL, 0, NULL, 2, b);

	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = color_view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE };
	VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth_view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue.depthStencil = { 1.0f, 0 } };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
		.layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
		.pDepthAttachment = &da, .pStencilAttachment = &da };
	BeginRendering(cmd, &ri);

	/* defaults for every dynamic state */
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkDeviceSize off = 0, stride = 16;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	SetViewportWithCount(cmd, 1, &vp);
	SetScissorWithCount(cmd, 1, &sc);
	SetCullMode(cmd, VK_CULL_MODE_NONE);
	SetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	SetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	SetDepthTestEnable(cmd, VK_FALSE);
	SetDepthWriteEnable(cmd, VK_FALSE);
	SetDepthCompareOp(cmd, VK_COMPARE_OP_LESS);
	SetStencilTestEnable(cmd, VK_FALSE);
	SetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
		     VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
	SetRasterizerDiscardEnable(cmd, VK_FALSE);
	SetDepthBiasEnable(cmd, VK_FALSE);
	SetPrimitiveRestartEnable(cmd, VK_FALSE);
	BindVertexBuffers2(cmd, 0, 1, &vb, &off, NULL, &stride);
	return cmd;
}

static void draw(VkCommandBuffer cmd, int first, int count, float r, float g, float b)
{
	float c[4] = { r, g, b, 1 };

	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
	vkCmdDraw(cmd, count, 1, first, 0);
}

static void finish(VkCommandBuffer cmd)
{
	EndRendering(cmd);
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = color, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

/* 'k' black, 'r' red, 'g' green, 'b' blue */
static char px(int x, int y)
{
	const uint8_t *p = rb_ptr + (y * W + x) * 4;

	if (p[0] > 200 && p[1] < 50 && p[2] < 50) return 'r';
	if (p[0] < 50 && p[1] > 200 && p[2] < 50) return 'g';
	if (p[0] < 50 && p[1] < 50 && p[2] > 200) return 'b';
	if (p[0] < 50 && p[1] < 50 && p[2] < 50) return 'k';
	return '?';
}

static void expect(const char *name, int x, int y, char want)
{
	char got = px(x, y);

	if (got != want) {
		printf("FAIL %-28s pixel (%d,%d) = %c, want %c\n", name, x, y, got, want);
		failures++;
	} else {
		printf("ok   %-28s pixel (%d,%d) = %c\n", name, x, y, got);
	}
}

int main(void)
{
	VkCommandBuffer cmd;
	char a, b;

	setvbuf(stdout, NULL, _IONBF, 0);
	init();

	cmd = begin();
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("basic", 32, 32, 'r');

	/* cull mode / front face */
	cmd = begin();
	SetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
	SetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	a = px(32, 32);
	cmd = begin();
	SetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
	SetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	b = px(32, 32);
	printf("%s cull back: front CCW -> %c, front CW -> %c\n", (a == 'r') != (b == 'r') ? "ok  " : "FAIL", a, b);
	failures += (a == 'r') == (b == 'r');
	cmd = begin();
	SetCullMode(cmd, VK_CULL_MODE_FRONT_AND_BACK);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("cull front+back", 32, 32, 'k');
	cmd = begin();
	SetCullMode(cmd, a == 'r' ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("cull other face", 32, 32, 'k');

	/* depth test / compare / write */
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_TRUE);
	draw(cmd, VB_QUAD02, 6, 0, 1, 0);
	draw(cmd, VB_QUAD08, 6, 1, 0, 0);
	finish(cmd);
	expect("depth test less", 32, 32, 'g');
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_TRUE);
	draw(cmd, VB_QUAD02, 6, 0, 1, 0);
	SetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER);
	draw(cmd, VB_QUAD08, 6, 1, 0, 0);
	finish(cmd);
	expect("depth compare greater", 32, 32, 'r');
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_TRUE);
	draw(cmd, VB_QUAD02, 6, 0, 1, 0);
	SetDepthTestEnable(cmd, VK_FALSE);
	draw(cmd, VB_QUAD08, 6, 1, 0, 0);
	finish(cmd);
	expect("depth test off", 32, 32, 'r');
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_FALSE);
	draw(cmd, VB_QUAD02, 6, 0, 1, 0);
	draw(cmd, VB_QUAD08, 6, 1, 0, 0);
	finish(cmd);
	expect("depth write off", 32, 32, 'r');

	/* depth bias enable (static bias 100000 units) */
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_TRUE);
	SetDepthCompareOp(cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
	draw(cmd, VB_QUAD05, 6, 0, 1, 0);
	SetDepthBiasEnable(cmd, VK_TRUE);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("depth bias on", 32, 32, 'g');
	cmd = begin();
	SetDepthTestEnable(cmd, VK_TRUE);
	SetDepthWriteEnable(cmd, VK_TRUE);
	SetDepthCompareOp(cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
	draw(cmd, VB_QUAD05, 6, 0, 1, 0);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("depth bias off", 32, 32, 'r');

	/* stencil test / ops */
	cmd = begin();
	SetStencilTestEnable(cmd, VK_TRUE);
	SetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
		     VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1);
	draw(cmd, VB_LEFT, 6, 0, 1, 0);
	SetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
		     VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL);
	draw(cmd, VB_QUAD05, 6, 0, 0, 1);
	finish(cmd);
	expect("stencil equal (marked)", 16, 32, 'b');
	expect("stencil equal (unmarked)", 48, 32, 'k');
	cmd = begin();
	SetStencilTestEnable(cmd, VK_TRUE);
	SetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
		     VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1);
	draw(cmd, VB_LEFT, 6, 0, 1, 0);
	SetStencilTestEnable(cmd, VK_FALSE);
	draw(cmd, VB_QUAD05, 6, 0, 0, 1);
	finish(cmd);
	expect("stencil off", 48, 32, 'b');

	/* rasterizer discard */
	cmd = begin();
	SetRasterizerDiscardEnable(cmd, VK_TRUE);
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	SetRasterizerDiscardEnable(cmd, VK_FALSE);
	draw(cmd, VB_LEFT, 6, 0, 1, 0);
	finish(cmd);
	expect("rasterizer discard", 48, 32, 'k');
	expect("rasterizer discard off", 16, 32, 'g');

	/* topology */
	cmd = begin();
	SetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	draw(cmd, VB_STRIP, 4, 1, 0, 0);
	finish(cmd);
	expect("topology strip", 56, 56, 'r');
	cmd = begin();
	draw(cmd, VB_STRIP, 3, 1, 0, 0);
	finish(cmd);
	expect("topology list", 56, 56, 'k');
	expect("topology list", 8, 8, 'r');

	/* viewport / scissor with count */
	cmd = begin();
	{
		VkViewport vp = { 0, 0, W / 2, H, 0, 1 };

		SetViewportWithCount(cmd, 1, &vp);
	}
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("viewport with count", 16, 32, 'r');
	expect("viewport with count", 48, 32, 'k');
	cmd = begin();
	{
		VkRect2D sc = { { 0, 0 }, { W, H / 2 } };

		SetScissorWithCount(cmd, 1, &sc);
	}
	draw(cmd, VB_QUAD05, 6, 1, 0, 0);
	finish(cmd);
	expect("scissor with count", 32, 16, 'r');
	expect("scissor with count", 32, 48, 'k');

	/* vertex binding stride */
	cmd = begin();
	{
		VkDeviceSize off = VB_WIDE * 16, stride = 32;

		BindVertexBuffers2(cmd, 0, 1, &vb, &off, NULL, &stride);
	}
	draw(cmd, 0, 6, 1, 0, 0);
	finish(cmd);
	expect("vertex stride 32", 32, 32, 'r');

	/* primitive restart */
	cmd = begin();
	SetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	SetPrimitiveRestartEnable(cmd, VK_TRUE);
	vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
	{
		float c[4] = { 1, 0, 0, 1 };

		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
	}
	vkCmdDrawIndexed(cmd, 7, 1, 0, VB_CORNERS, 0);
	finish(cmd);
	expect("primitive restart", 2, 2, 'r');
	expect("primitive restart", 61, 61, 'r');
	expect("primitive restart", 32, 32, 'k');

	printf(failures ? "FAILED: %d\n" : "PASS: all extended dynamic state cases\n", failures);
	return failures != 0;
}
