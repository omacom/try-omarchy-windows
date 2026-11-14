/* bltest: the Vulkan features Blender's Vulkan backend requires beyond 1.3,
 * checked by reading pixels back: dual-source blending, logic ops (XOR on a
 * normalized and an integer target), several viewports selected from the
 * vertex shader and from a geometry shader (with the per-viewport Y flip),
 * and VK_EXT_provoking_vertex (last vertex, triangle lists and strips, float
 * and integer flat inputs).
 *
 *   for s in bl.vert bl.frag blint.frag dual.frag uint.frag vpvs.vert vpgs.vert vpgs.geom; do
 *       glslc --target-env=vulkan1.3 $s -o $s.spv; done
 *   gcc -O2 -o bltest bltest.c -lvulkan && ./bltest
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static VkPipelineLayout layout;
static VkBuffer vb, rb;
static float *vbp;
static uint8_t *rbp;
static int failures;

struct target {
	VkFormat fmt;
	VkImage img;
	VkImageView view;
};
static struct target unorm = { VK_FORMAT_R8G8B8A8_UNORM }, uintt = { VK_FORMAT_R8G8B8A8_UINT };

/* vertex: pos.xyz, color.rgb, id */
#define STRIDE 28
static void vtx(int i, float x, float y, float r, float g, float b, int id)
{
	float *v = vbp + i * 7;

	v[0] = x; v[1] = y; v[2] = 0.5f; v[3] = r; v[4] = g; v[5] = b;
	memcpy(&v[6], &id, 4);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags f)
{
	VkPhysicalDeviceMemoryProperties mp;

	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f)
			return i;
	exit(1);
}

static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **ptr)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkMemoryRequirements req;
	VkDeviceMemory mem;
	VkBuffer b;

	CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
	vkGetBufferMemoryRequirements(dev, b, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
							   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, b, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, ptr));
	return b;
}

static void target_init(struct target *t)
{
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = t->fmt, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkMemoryRequirements req;
	VkDeviceMemory mem;

	CHECK(vkCreateImage(dev, &ii, NULL, &t->img));
	vkGetImageMemoryRequirements(dev, t->img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, t->img, mem, 0));
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = t->fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vi, NULL, &t->view));
}

static VkShaderModule shader(const char *name)
{
	static uint32_t code[16384];
	char path[256];
	FILE *f;
	size_t n;
	VkShaderModule m;

	snprintf(path, sizeof(path), "%s.spv", name);
	f = fopen(path, "rb");
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

struct pipe_desc {
	const char *vs, *gs, *fs;
	VkFormat fmt;
	VkPrimitiveTopology topo;
	VkPipelineColorBlendAttachmentState blend;
	VkBool32 logic;
	VkLogicOp logic_op;
	int last_vertex;
};

static VkPipeline pipeline(const struct pipe_desc *d, VkResult *res)
{
	VkPipelineShaderStageCreateInfo st[3];
	uint32_t n = 0;

	st[n++] = (VkPipelineShaderStageCreateInfo) { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(d->vs), .pName = "main" };
	if (d->gs)
		st[n++] = (VkPipelineShaderStageCreateInfo) { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = shader(d->gs), .pName = "main" };
	st[n++] = (VkPipelineShaderStageCreateInfo) { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(d->fs), .pName = "main" };
	VkVertexInputBindingDescription bind = { 0, STRIDE, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
		{ 2, 0, VK_FORMAT_R32_SINT, 24 },
	};
	VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
		.vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attr };
	VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = d->topo };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 2, .scissorCount = 2 };
	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
		.provokingVertexMode = d->last_vertex ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT :
						       VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, &pv,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable = d->logic, .logicOp = d->logic_op, .attachmentCount = 1, .pAttachments = &d->blend };
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dyn };
	VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1, .pColorAttachmentFormats = &d->fmt };
	VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri,
		.stageCount = n, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cbs, .pDynamicState = &dys, .layout = layout };
	VkPipeline p = VK_NULL_HANDLE;

	*res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &p);
	return p;
}

/* Clears the target to `clear`, draws `count` vertices from `first` with
 * `instances` instances into two half-width viewports (or one full one),
 * and reads the target back. */
static void render(VkPipeline p, struct target *t, VkClearColorValue clear, int first, int count,
		   int instances, int two_viewports)
{
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = t->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     0, 0, NULL, 0, NULL, 1, &b);
	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color = clear };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
		.layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
	vkCmdBeginRendering(cmd, &ri);
	VkViewport vp[2] = { { 0, 0, W, H, 0, 1 }, { 0, 0, W, H, 0, 1 } };
	VkRect2D sc[2] = { { { 0, 0 }, { W, H } }, { { 0, 0 }, { W, H } } };
	if (two_viewports) {
		vp[0].width = vp[1].width = W / 2;
		vp[1].x = W / 2;
		sc[0].extent.width = sc[1].extent.width = W / 2;
		sc[1].offset.x = W / 2;
	}
	VkDeviceSize off = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	vkCmdSetViewport(cmd, 0, 2, vp);
	vkCmdSetScissor(cmd, 0, 2, sc);
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
	vkCmdDraw(cmd, count, instances, first, 0);
	vkCmdEndRendering(cmd);
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, cpool, 1, &cmd);
}

static void expect(const char *name, int x, int y, int r, int g, int b, int a)
{
	const uint8_t *p = rbp + (y * W + x) * 4;
	int ok = abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3 && (a < 0 || abs(p[3] - a) <= 3);

	printf("%s %-34s (%2d,%2d) = %3d %3d %3d %3d", ok ? "ok  " : "FAIL", name, x, y, p[0], p[1], p[2], p[3]);
	if (!ok)
		printf("   want %d %d %d %d", r, g, b, a);
	printf("\n");
	failures += !ok;
}

static const VkPipelineColorBlendAttachmentState plain = { .colorWriteMask = 0xf };

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	uint32_t n = 1;
	VkResult res;
	VkPipeline p;

	setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	vkEnumeratePhysicalDevices(inst, &n, &pd);

	VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
	VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &pvf };
	VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	printf("multiViewport %u logicOp %u dualSrcBlend %u provokingVertexLast %u "
	       "shaderOutputViewportIndex %u shaderOutputLayer %u geometryShader %u\n",
	       f2.features.multiViewport, f2.features.logicOp, f2.features.dualSrcBlend,
	       pvf.provokingVertexLast, f12.shaderOutputViewportIndex, f12.shaderOutputLayer,
	       f2.features.geometryShader);
	if (!f2.features.multiViewport || !f2.features.logicOp || !f2.features.dualSrcBlend ||
	    !pvf.provokingVertexLast) {
		printf("FAIL: features Blender needs are missing\n");
		return 1;
	}

	float prio = 1;
	const char *ext = "VK_EXT_provoking_vertex";
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext,
		.pEnabledFeatures = &f2.features };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &cpool));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	target_init(&unorm);
	target_init(&uintt);
	vb = buffer(64 * STRIDE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vbp);
	rb = buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rbp);

	/* 0-5: full-screen quad (two triangles); 6-8: big triangle red/green/blue;
	 * 9-12: strip TL red, TR green, BL blue, BR white; 13-18: top-half quad */
	vtx(0, -1, -1, 1, 1, 1, 3); vtx(1, 1, -1, 1, 1, 1, 3); vtx(2, -1, 1, 1, 1, 1, 3);
	vtx(3, 1, -1, 1, 1, 1, 3); vtx(4, 1, 1, 1, 1, 1, 3); vtx(5, -1, 1, 1, 1, 1, 3);
	vtx(6, -1, -1, 1, 0, 0, 0); vtx(7, 3, -1, 0, 1, 0, 1); vtx(8, -1, 3, 0, 0, 1, 2);
	vtx(9, -1, -1, 1, 0, 0, 0); vtx(10, 1, -1, 0, 1, 0, 1); vtx(11, -1, 1, 0, 0, 1, 2);
	vtx(12, 1, 1, 1, 1, 1, 3);
	vtx(13, -1, -1, 1, 1, 1, 3); vtx(14, 1, -1, 1, 1, 1, 3); vtx(15, -1, 0, 1, 1, 1, 3);
	vtx(16, 1, -1, 1, 1, 1, 3); vtx(17, 1, 0, 1, 1, 1, 3); vtx(18, -1, 0, 1, 1, 1, 3);

	VkClearColorValue grey = { .float32 = { 0.5f, 0.5f, 0.5f, 1 } }, black = { 0 };

	/* dual-source: dst = src0 + dst * src1 */
	struct pipe_desc dual = { "bl.vert", NULL, "dual.frag", VK_FORMAT_R8G8B8A8_UNORM,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		{ VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf } };
	p = pipeline(&dual, &res);
	if (res != VK_SUCCESS) {
		printf("FAIL dual-source pipeline: %d\n", res);
		failures++;
	} else {
		render(p, &unorm, grey, 0, 6, 1, 0);
		expect("dual-source blend", 32, 32, 191, 0, 64, 255);
	}

	/* logic op XOR */
	struct pipe_desc lx = { "bl.vert", NULL, "bl.frag", VK_FORMAT_R8G8B8A8_UNORM,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain, VK_TRUE, VK_LOGIC_OP_XOR };
	VkClearColorValue pattern = { .float32 = { 0x0f / 255.0f, 0xf0 / 255.0f, 0x55 / 255.0f, 1 } };
	p = pipeline(&lx, &res);
	if (res != VK_SUCCESS) {
		printf("FAIL logic op pipeline (UNORM): %d\n", res);
		failures++;
	} else {
		render(p, &unorm, pattern, 0, 6, 1, 0);	/* white ^ pattern */
		expect("logic op XOR (UNORM)", 32, 32, 0xf0, 0x0f, 0xaa, 0);
	}
	struct pipe_desc lu = { "bl.vert", NULL, "uint.frag", VK_FORMAT_R8G8B8A8_UINT,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain, VK_TRUE, VK_LOGIC_OP_XOR };
	VkClearColorValue upattern = { .uint32 = { 0x0f, 0xf0, 0x55, 0xff } };
	p = pipeline(&lu, &res);
	if (res != VK_SUCCESS) {
		printf("FAIL logic op pipeline (UINT): %d\n", res);
		failures++;
	} else {
		render(p, &uintt, upattern, 0, 6, 1, 0);	/* 0xff,0xff,0x00,0xff ^ pattern */
		expect("logic op XOR (UINT)", 32, 32, 0xf0, 0x0f, 0x55, 0);
	}

	/* two viewports: top-half quad, instance i to viewport i (vertex shader) */
	struct pipe_desc vvs = { "vpvs.vert", NULL, "bl.frag", VK_FORMAT_R8G8B8A8_UNORM,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain };
	p = pipeline(&vvs, &res);
	if (res != VK_SUCCESS) {
		printf("FAIL viewport-index vertex shader pipeline: %d\n", res);
		failures++;
	} else {
		render(p, &unorm, black, 13, 6, 2, 1);
		expect("viewport 0 from VS, top", 16, 16, 255, 0, 0, 255);
		expect("viewport 1 from VS, top", 48, 16, 0, 255, 0, 255);
		expect("viewport 0 from VS, bottom", 16, 48, 0, 0, 0, -1);
		expect("viewport 1 from VS, bottom", 48, 48, 0, 0, 0, -1);
	}
	/* geometry shader without a viewport index, and with a constant one */
	{
		struct pipe_desc g0 = { "vpgs.vert", "gs0.geom", "bl.frag", VK_FORMAT_R8G8B8A8_UNORM,
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain };
		struct pipe_desc g1 = g0;

		g1.gs = "gs1.geom";
		p = pipeline(&g0, &res);
		render(p, &unorm, black, 13, 6, 1, 1);
		expect("GS, no viewport index, vp 0 top", 16, 16, 255, 0, 0, 255);
		p = pipeline(&g1, &res);
		render(p, &unorm, black, 13, 6, 1, 1);
		expect("GS, viewport index 1, vp 1 top", 48, 16, 0, 255, 0, 255);
		expect("GS, viewport index 1, vp 0 top", 16, 16, 0, 0, 0, -1);
	}
	/* the same through a geometry shader that sends each triangle to both */
	struct pipe_desc vgs = { "vpgs.vert", "vpgs.geom", "bl.frag", VK_FORMAT_R8G8B8A8_UNORM,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain };
	p = pipeline(&vgs, &res);
	if (res != VK_SUCCESS) {
		printf("FAIL viewport-index geometry shader pipeline: %d\n", res);
		failures++;
	} else {
		render(p, &unorm, black, 13, 6, 1, 1);
		expect("viewport 0 from GS, top", 16, 16, 255, 0, 0, 255);
		expect("viewport 1 from GS, top", 48, 16, 0, 255, 0, 255);
		expect("viewport 0 from GS, bottom", 16, 48, 0, 0, 0, -1);
		expect("viewport 1 from GS, bottom", 48, 48, 0, 0, 0, -1);
	}

	/* provoking vertex */
	for (int last = 0; last < 2; last++) {
		for (int integer = 0; integer < 2; integer++) {
			char name[64];
			struct pipe_desc pl = { "bl.vert", NULL, integer ? "blint.frag" : "bl.frag",
				VK_FORMAT_R8G8B8A8_UNORM, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, plain,
				.last_vertex = last };
			struct pipe_desc ps = pl;

			ps.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			p = pipeline(&pl, &res);
			if (res != VK_SUCCESS) {
				printf("FAIL provoking vertex pipeline: %d\n", res);
				failures++;
				continue;
			}
			render(p, &unorm, black, 6, 3, 1, 0);
			snprintf(name, sizeof(name), "%s vertex, list, %s", last ? "last" : "first",
				 integer ? "int" : "float");
			if (last)
				expect(name, 8, 8, 0, 0, 255, 255);
			else
				expect(name, 8, 8, 255, 0, 0, 255);
			p = pipeline(&ps, &res);
			render(p, &unorm, black, 9, 4, 1, 0);
			snprintf(name, sizeof(name), "%s vertex, strip even, %s", last ? "last" : "first",
				 integer ? "int" : "float");
			if (last)
				expect(name, 8, 8, 0, 0, 255, 255);
			else
				expect(name, 8, 8, 255, 0, 0, 255);
			snprintf(name, sizeof(name), "%s vertex, strip odd, %s", last ? "last" : "first",
				 integer ? "int" : "float");
			if (last)
				expect(name, 56, 56, 255, 255, 255, 255);
			else
				expect(name, 56, 56, 0, 255, 0, 255);
		}
	}

	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf(failures ? "FAILED: %d\n" : "PASS: Blender's Vulkan requirements\n", failures);
	return failures != 0;
}
