/* vk13test: Vulkan 1.3 pieces of Dozen that need more than a flag:
 * inline uniform blocks (write, partial write, update template, copy),
 * zero-initialized workgroup memory, maintenance4 requirements queries and
 * VkFormatProperties3.
 *
 *   glslc --target-env=vulkan1.3 inline.comp -o inline.spv
 *   glslc --target-env=vulkan1.3 dirty.comp -o dirty.spv
 *   glslc --target-env=vulkan1.3 zero.comp -o zero.spv
 *   gcc -O2 -o vk13test vk13test.c -lvulkan && ./vk13test
 *
 * The device enables every feature, which puts Dozen in its bindless
 * descriptor mode; NOBINDLESS=1 leaves out descriptor indexing and buffer
 * addresses (descriptor-heap mode). LEAK=1 exits without destroying the
 * device, as a killed application does.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue queue;
static VkCommandPool cpool;
static int failures;

static void result(const char *name, int ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", name);
	failures += !ok;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties mp;

	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	exit(1);
}

static VkBuffer make_buffer(VkDeviceSize size, void **ptr)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkMemoryRequirements req;
	VkDeviceMemory mem;
	VkBuffer buf;

	CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
							   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, ptr));
	memset(*ptr, 0xcc, size);
	return buf;
}

static VkPipeline make_pipeline(const char *path, VkPipelineLayout layout)
{
	static uint32_t code[16384];
	FILE *f = fopen(path, "rb");
	size_t n;
	VkShaderModule m;
	VkPipeline p;

	if (!f) {
		printf("FAIL open %s\n", path);
		exit(1);
	}
	n = fread(code, 1, sizeof(code), f);
	fclose(f);
	VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
	CHECK(vkCreateShaderModule(dev, &si, NULL, &m));
	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			   .module = m, .pName = "main" },
		.layout = layout };
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
	return p;
}

static void dispatch(VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set)
{
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

	CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &bi));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cmd, 1, 1, 1);
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb,
			     0, NULL, 0, NULL);
	CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, cpool, 1, &cmd);
}

static int check4(const uint32_t *got, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (got[0] == a && got[1] == b && got[2] == c && got[3] == d)
		return 1;
	printf("     got %u %u %u %u, want %u %u %u %u\n", got[0], got[1], got[2], got[3], a, b, c, d);
	return 0;
}

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	uint32_t n = 1;
	VkPhysicalDeviceProperties props;

	setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
	       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

	VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	printf("1.3 features: robustImageAccess %u inlineUniformBlock %u pipelineCreationCacheControl %u "
	       "privateData %u demote %u terminate %u subgroupSizeControl %u computeFullSubgroups %u "
	       "synchronization2 %u zeroInitWorkgroup %u dynamicRendering %u dotProduct %u maintenance4 %u\n",
	       f13.robustImageAccess, f13.inlineUniformBlock, f13.pipelineCreationCacheControl, f13.privateData,
	       f13.shaderDemoteToHelperInvocation, f13.shaderTerminateInvocation, f13.subgroupSizeControl,
	       f13.computeFullSubgroups, f13.synchronization2, f13.shaderZeroInitializeWorkgroupMemory,
	       f13.dynamicRendering, f13.shaderIntegerDotProduct, f13.maintenance4);
	printf("1.2 required by 1.3: vulkanMemoryModel %u deviceScope %u bufferDeviceAddress %u\n",
	       f12.vulkanMemoryModel, f12.vulkanMemoryModelDeviceScope, f12.bufferDeviceAddress);
	result("all Vulkan 1.3 required features",
	       f13.robustImageAccess && f13.inlineUniformBlock && f13.pipelineCreationCacheControl &&
	       f13.privateData && f13.shaderDemoteToHelperInvocation && f13.shaderTerminateInvocation &&
	       f13.subgroupSizeControl && f13.computeFullSubgroups && f13.synchronization2 &&
	       f13.shaderZeroInitializeWorkgroupMemory && f13.dynamicRendering &&
	       f13.shaderIntegerDotProduct && f13.maintenance4 && f12.vulkanMemoryModel &&
	       f12.vulkanMemoryModelDeviceScope && f12.bufferDeviceAddress);

	f13.pNext = NULL;
	f12.pNext = &f13;
	float prio = 1;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi };
	f2.pNext = NULL;
	dci.pEnabledFeatures = &f2.features;
	if (getenv("NOBINDLESS")) {
		/* Dozen goes bindless for descriptor indexing or buffer addresses */
		f12.descriptorIndexing = f12.bufferDeviceAddress = VK_FALSE;
		f12.shaderUniformTexelBufferArrayDynamicIndexing = f12.shaderStorageTexelBufferArrayDynamicIndexing = 0;
		f12.shaderUniformBufferArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing = 0;
		f12.shaderStorageBufferArrayNonUniformIndexing = f12.shaderStorageImageArrayNonUniformIndexing = 0;
		f12.shaderInputAttachmentArrayNonUniformIndexing = 0;
		f12.shaderUniformTexelBufferArrayNonUniformIndexing = f12.shaderStorageTexelBufferArrayNonUniformIndexing = 0;
		f12.descriptorBindingUniformBufferUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind = 0;
		f12.descriptorBindingStorageImageUpdateAfterBind = f12.descriptorBindingStorageBufferUpdateAfterBind = 0;
		f12.descriptorBindingUniformTexelBufferUpdateAfterBind = 0;
		f12.descriptorBindingStorageTexelBufferUpdateAfterBind = 0;
		f12.descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingPartiallyBound = 0;
		f12.descriptorBindingVariableDescriptorCount = f12.runtimeDescriptorArray = 0;
		f13.descriptorBindingInlineUniformBlockUpdateAfterBind = 0;
	}
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &cpool));

	/* ---- inline uniform blocks ---- */
	uint32_t *out;
	VkBuffer obuf = make_buffer(4096, (void **)&out);
	VkDescriptorSetLayoutBinding lb[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 16, VK_SHADER_STAGE_COMPUTE_BIT },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2, .pBindings = lb };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkPipeline ipipe = make_pipeline("inline.spv", pl);

	VkDescriptorPoolInlineUniformBlockCreateInfo piub = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO, .maxInlineUniformBlockBindings = 4 };
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 64 },
				       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, &piub,
		.maxSets = 4, .poolSizeCount = 2, .pPoolSizes = ps };
	VkDescriptorPool dpool;
	CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
	VkDescriptorSetLayout dsls[3] = { dsl, dsl, dsl };
	VkDescriptorSet sets[3];
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 3, .pSetLayouts = dsls };
	CHECK(vkAllocateDescriptorSets(dev, &dsai, sets));

	/* set 0: full write then a partial write at byte 8 */
	uint32_t v0[4] = { 1, 2, 3, 4 }, v1[2] = { 7, 8 };
	VkWriteDescriptorSetInlineUniformBlock w0 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
		.dataSize = 16, .pData = v0 };
	VkWriteDescriptorSetInlineUniformBlock w1 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
		.dataSize = 8, .pData = v1 };
	VkDescriptorBufferInfo bi0 = { obuf, 0, 256 }, bi2 = { obuf, 512, 256 };
	VkWriteDescriptorSet ws[4] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &w0, sets[0], 0, 0, 16,
		  VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &w1, sets[0], 0, 8, 8,
		  VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 1, 0, 1,
		  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi0 },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[2], 1, 0, 1,
		  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi2 },
	};
	vkUpdateDescriptorSets(dev, 4, ws, 0, NULL);

	/* set 1: update template (inline bytes + the storage buffer) */
	struct {
		uint32_t v[4];
		VkDescriptorBufferInfo b;
	} tdata = { { 5, 6, 7, 8 }, { obuf, 256, 256 } };
	VkDescriptorUpdateTemplateEntry te[2] = {
		{ 0, 0, 16, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 0, 0 },
		{ 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(typeof(tdata), b), 0 },
	};
	VkDescriptorUpdateTemplateCreateInfo tci = { VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
		.descriptorUpdateEntryCount = 2, .pDescriptorUpdateEntries = te,
		.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = dsl };
	VkDescriptorUpdateTemplate templ;
	CHECK(vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &templ));
	vkUpdateDescriptorSetWithTemplate(dev, sets[1], templ, &tdata);

	/* set 2: copy of set 0's block */
	VkCopyDescriptorSet cp = { VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, sets[0], 0, 0, sets[2], 0, 0, 16 };
	vkUpdateDescriptorSets(dev, 0, NULL, 1, &cp);

	for (int s = 0; s < 3; s++)
		dispatch(ipipe, pl, sets[s]);
	result("inline uniform block write + partial write", check4(out, 1, 2, 7, 8));
	result("inline uniform block update template", check4(out + 64, 5, 6, 7, 8));
	result("inline uniform block copy", check4(out + 128, 1, 2, 7, 8));

	/* ---- zero-initialized workgroup memory ---- */
	VkDescriptorSetLayoutBinding zb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo zlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &zb };
	VkDescriptorSetLayout zdsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &zlci, NULL, &zdsl));
	VkPipelineLayoutCreateInfo zplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &zdsl };
	VkPipelineLayout zpl;
	CHECK(vkCreatePipelineLayout(dev, &zplci, NULL, &zpl));
	VkPipeline dirty = make_pipeline("dirty.spv", zpl), zero = make_pipeline("zero.spv", zpl);
	VkDescriptorPoolSize zps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
	VkDescriptorPoolCreateInfo zdpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &zps };
	VkDescriptorPool zpool;
	CHECK(vkCreateDescriptorPool(dev, &zdpci, NULL, &zpool));
	VkDescriptorSet zset;
	VkDescriptorSetAllocateInfo zdsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = zpool, .descriptorSetCount = 1, .pSetLayouts = &zdsl };
	CHECK(vkAllocateDescriptorSets(dev, &zdsai, &zset));
	uint32_t *zout;
	VkBuffer zbuf = make_buffer(4096, (void **)&zout);
	VkDescriptorBufferInfo zbi = { zbuf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet zw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, zset, 0, 0, 1,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &zbi };
	vkUpdateDescriptorSets(dev, 1, &zw, 0, NULL);
	dispatch(dirty, zpl, zset);
	dispatch(zero, zpl, zset);
	{
		int ok = 1;

		for (int i = 0; i < 256; i++)
			if (zout[i] != 1) {
				printf("     shared[%d] = %u, want 1 (was 0)\n", i, zout[i]);
				ok = 0;
				break;
			}
		result("zero-initialized workgroup memory", ok);
	}

	/* ---- maintenance4 ---- */
	{
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 100000,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
		VkDeviceBufferMemoryRequirements dbr = { VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
			.pCreateInfo = &bci };
		VkMemoryRequirements2 r1 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
		VkMemoryRequirements r2;
		VkBuffer b;

		vkGetDeviceBufferMemoryRequirements(dev, &dbr, &r1);
		CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
		vkGetBufferMemoryRequirements(dev, b, &r2);
		result("maintenance4 buffer requirements",
		       r1.memoryRequirements.size == r2.size && r1.memoryRequirements.size >= 100000 &&
		       r1.memoryRequirements.memoryTypeBits == r2.memoryTypeBits);

		VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1000, 500, 1 }, .mipLevels = 3, .arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
		VkDeviceImageMemoryRequirements dir = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
			.pCreateInfo = &ii };
		VkMemoryRequirements2 i1 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
		VkMemoryRequirements i2;
		VkImage img;

		vkGetDeviceImageMemoryRequirements(dev, &dir, &i1);
		CHECK(vkCreateImage(dev, &ii, NULL, &img));
		vkGetImageMemoryRequirements(dev, img, &i2);
		result("maintenance4 image requirements",
		       i1.memoryRequirements.size == i2.size && i2.size >= 2000000 &&
		       i1.memoryRequirements.alignment == i2.alignment);
	}

	/* ---- VkFormatProperties3 ---- */
	{
		VkFormatProperties3 p3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
		VkFormatProperties2 p2 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3 };

		vkGetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_R8G8B8A8_UNORM, &p2);
		result("format properties 3 (rgba8 storage without format)",
		       (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) &&
		       (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) &&
		       (uint32_t)p3.optimalTilingFeatures == (p2.formatProperties.optimalTilingFeatures |
			(uint32_t)(p3.optimalTilingFeatures & 0xffffffffu & ~p2.formatProperties.optimalTilingFeatures)));
		vkGetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_D32_SFLOAT, &p2);
		result("format properties 3 (depth comparison)",
		       !!(p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT));
	}

	if (getenv("LEAK")) {
		printf(failures ? "FAILED: %d (leaking the device)\n" : "PASS (leaking the device)\n", failures);
		return failures != 0;
	}
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf(failures ? "FAILED: %d\n" : "PASS: Vulkan 1.3 checks\n", failures);
	return failures != 0;
}
