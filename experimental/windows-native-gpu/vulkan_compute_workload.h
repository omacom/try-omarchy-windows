// SPDX-License-Identifier: MIT
// GPU-only storage writes, GPU transfer, fence retirement and full integer oracle.
// Synchronization follows the Khronos compute/readback example:
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
#pragma once
#include "vulkan_compute_shader.inc" // Generated from the pinned GLSL source.
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>

namespace {
constexpr std::uint32_t ComputeWidth = 3840, ComputeHeight = 2160;
constexpr VkDeviceSize ComputeBytes = VkDeviceSize(ComputeWidth) * ComputeHeight * sizeof(std::uint32_t);
struct ComputeObjects {
    Runtime& runtime;
    VkBuffer output = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool pending = false;
    explicit ComputeObjects(Runtime& value) : runtime(value) {}
    ComputeObjects(const ComputeObjects&) = delete;
    ~ComputeObjects() {
        const auto device = runtime.device;
        if (pending) runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
        if (mapped) runtime.proc<PFN_vkUnmapMemory>("vkUnmapMemory")(device, readbackMemory);
        if (fence) runtime.proc<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
        if (commands) runtime.proc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, commands, nullptr);
        if (descriptors) runtime.proc<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device, descriptors, nullptr);
        if (pipeline) runtime.proc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, pipeline, nullptr);
        if (pipelineLayout) runtime.proc<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, pipelineLayout, nullptr);
        if (descriptorLayout) runtime.proc<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(device, descriptorLayout, nullptr);
        if (shader) runtime.proc<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, shader, nullptr);
        if (output) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, output, nullptr);
        if (readback) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, readback, nullptr);
        if (outputMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(device, outputMemory, nullptr);
        if (readbackMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(device, readbackMemory, nullptr);
    }
};

std::uint32_t computeReference(std::uint32_t x, std::uint32_t y, unsigned round) {
    // Independent CPU implementation, evaluated for every returned word.
    const auto r = (13 * x + 7 * y + 31 * round) % 256;
    const auto g = ((x ^ (3 * y)) + 71 * round) % 256;
    const auto b = ((x / 17) ^ (y / 11) ^ (113 * round)) % 256;
    return r + 256 * g + 65536 * b + 0xff000000u;
}
void hashComputeWord(std::uint64_t& hash, std::uint32_t word) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (word >> shift) & 255u;
        hash *= UINT64_C(1099511628211);
    }
}

void computeWorkload(Runtime& runtime, VkPhysicalDevice physical, std::uint32_t family, VkQueue queue) {
    std::cout << "VULKAN_COMPUTE_BEGIN width=3840 height=2160 words=8294400 rounds=2 cpuStorageWrites=false window=false" << std::endl;
    VkPhysicalDeviceProperties properties{};
    runtime.proc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &properties);
    require(properties.limits.maxStorageBufferRange >= ComputeBytes &&
        properties.limits.maxComputeWorkGroupCount[0] >= ComputeWidth / 8 &&
        properties.limits.maxComputeWorkGroupCount[1] >= ComputeHeight / 8 &&
        properties.limits.maxComputeWorkGroupInvocations >= 64 &&
        properties.limits.maxComputeWorkGroupSize[0] >= 8 && properties.limits.maxComputeWorkGroupSize[1] >= 8,
        "insufficient 4K compute limits");
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    runtime.proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memoryProperties);
    require(memoryProperties.memoryTypeCount > 0 && memoryProperties.memoryTypeCount <= VK_MAX_MEMORY_TYPES,
        "invalid memory properties");
    ComputeObjects objects(runtime);
    bool coherent = false;
    auto buffer = [&](bool gpuOnly, VkBuffer& handle, VkDeviceMemory& memory) {
        VkBufferCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size = ComputeBytes;
        create.usage = gpuOnly ? VkBufferUsageFlags(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT) :
            VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result(gpuOnly ? "compute-output-buffer" : "compute-readback-buffer",
            runtime.proc<PFN_vkCreateBuffer>("vkCreateBuffer")(runtime.device, &create, nullptr, &handle));
        VkMemoryRequirements requirements{};
        runtime.proc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(runtime.device, handle, &requirements);
        require(requirements.size >= ComputeBytes && requirements.size <= 64 * 1024 * 1024,
            "unexpected compute buffer allocation size");
        std::uint32_t selected = memoryProperties.memoryTypeCount;
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if (!(requirements.memoryTypeBits & (1u << index))) continue;
            const auto flags = memoryProperties.memoryTypes[index].propertyFlags;
            const bool eligible = gpuOnly ? ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) :
                bool(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (!eligible) continue;
            if (selected == memoryProperties.memoryTypeCount || (!gpuOnly && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) selected = index;
            if (gpuOnly || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
        }
        require(selected < memoryProperties.memoryTypeCount, "required GPU-only/readback memory type absent");
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = selected;
        result(gpuOnly ? "compute-output-memory" : "compute-readback-memory",
            runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(runtime.device, &allocate, nullptr, &memory));
        result(gpuOnly ? "compute-output-bind" : "compute-readback-bind",
            runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(runtime.device, handle, memory, 0));
        if (!gpuOnly) coherent = (memoryProperties.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        std::cout << "VULKAN_COMPUTE_MEMORY role=" << (gpuOnly ? "output" : "readback")
            << " type=" << selected << " flags=" << memoryProperties.memoryTypes[selected].propertyFlags
            << " bytes=" << requirements.size << " cpuInitialized=false" << std::endl;
    };
    buffer(true, objects.output, objects.outputMemory);
    buffer(false, objects.readback, objects.readbackMemory);

    VkShaderModuleCreateInfo shader{};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof(VulkanComputeShader);
    shader.pCode = VulkanComputeShader;
    result("compute-shader-module", runtime.proc<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
        runtime.device, &shader, nullptr, &objects.shader));
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1; layout.pBindings = &binding;
    result("compute-descriptor-layout", runtime.proc<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
        runtime.device, &layout, nullptr, &objects.descriptorLayout));
    VkPushConstantRange constants{};
    constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; constants.size = 3 * sizeof(std::uint32_t);
    VkPipelineLayoutCreateInfo pipelineLayout{};
    pipelineLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &objects.descriptorLayout;
    pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &constants;
    result("compute-pipeline-layout", runtime.proc<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        runtime.device, &pipelineLayout, nullptr, &objects.pipelineLayout));
    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = objects.shader; pipeline.stage.pName = "main";
    pipeline.layout = objects.pipelineLayout;
    result("compute-pipeline", runtime.proc<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        runtime.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &objects.pipeline));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &poolSize;
    result("compute-descriptor-pool", runtime.proc<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
        runtime.device, &pool, nullptr, &objects.descriptors));
    VkDescriptorSetAllocateInfo descriptor{};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor.descriptorPool = objects.descriptors; descriptor.descriptorSetCount = 1;
    descriptor.pSetLayouts = &objects.descriptorLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    result("compute-descriptor-set", runtime.proc<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
        runtime.device, &descriptor, &set));
    VkDescriptorBufferInfo bufferInfo{objects.output, 0, ComputeBytes};
    VkWriteDescriptorSet update{};
    update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    update.dstSet = set; update.descriptorCount = 1;
    update.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; update.pBufferInfo = &bufferInfo;
    runtime.proc<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(runtime.device, 1, &update, 0, nullptr);
    VkCommandPoolCreateInfo commandPool{};
    commandPool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPool.queueFamilyIndex = family; commandPool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result("compute-command-pool", runtime.proc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
        runtime.device, &commandPool, nullptr, &objects.commands));
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = objects.commands; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result("compute-command-buffer", runtime.proc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
        runtime.device, &commandInfo, &command));
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result("compute-fence", runtime.proc<PFN_vkCreateFence>("vkCreateFence")(runtime.device, &fence, nullptr, &objects.fence));
    const auto barrier = [&](VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage,
                             VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
        VkMemoryBarrier memory{}; memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memory.srcAccessMask = sourceAccess; memory.dstAccessMask = destinationAccess;
        runtime.proc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, sourceStage, destinationStage,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    };
    std::uint64_t previous = 0;
    for (unsigned round = 0; round < 2; ++round) {
        if (round) result("compute-reset-command", runtime.proc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0));
        result("compute-reset-fence", runtime.proc<PFN_vkResetFences>("vkResetFences")(runtime.device, 1, &objects.fence));
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result("compute-begin-command", runtime.proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin));
        if (round) {
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        runtime.proc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_COMPUTE, objects.pipeline);
        runtime.proc<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipelineLayout, 0, 1, &set, 0, nullptr);
        const std::array<std::uint32_t, 3> dimensions{ComputeWidth, ComputeHeight, round};
        runtime.proc<PFN_vkCmdPushConstants>("vkCmdPushConstants")(command, objects.pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dimensions), dimensions.data());
        runtime.proc<PFN_vkCmdDispatch>("vkCmdDispatch")(command, ComputeWidth / 8, ComputeHeight / 8, 1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{0, 0, ComputeBytes};
        runtime.proc<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(command, objects.output, objects.readback, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        result("compute-end-command", runtime.proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command));
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        objects.pending = true;
        result("compute-queue-submit", runtime.proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, objects.fence));
        // Submission is complete before the wait begins: the fence is not
        // accessed concurrently with QueueSubmit on a second host thread.
        result("compute-wait-fence", runtime.proc<PFN_vkWaitForFences>("vkWaitForFences")(
            runtime.device, 1, &objects.fence, VK_TRUE, UINT64_C(30000000000)));
        objects.pending = false;
        result("compute-map-readback", runtime.proc<PFN_vkMapMemory>("vkMapMemory")(
            runtime.device, objects.readbackMemory, 0, VK_WHOLE_SIZE, 0, &objects.mapped));
        require(objects.mapped != nullptr, "null compute readback mapping");
        if (!coherent) {
            VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = objects.readbackMemory; range.size = VK_WHOLE_SIZE;
            result("compute-invalidate-readback", runtime.proc<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(
                runtime.device, 1, &range));
        }
        const auto words = static_cast<const std::uint32_t*>(objects.mapped);
        std::uint64_t actualHash = UINT64_C(14695981039346656037), referenceHash = actualHash;
        for (std::uint32_t y = 0; y < ComputeHeight; ++y) {
            for (std::uint32_t x = 0; x < ComputeWidth; ++x) {
                const auto actual = words[std::size_t(y) * ComputeWidth + x];
                const auto expected = computeReference(x, y, round);
                if (actual != expected) {
                    std::cerr << "VULKAN_COMPUTE_MISMATCH round=" << round << " x=" << x << " y=" << y
                        << " actual=" << actual << " expected=" << expected << std::endl;
                    throw std::runtime_error("compute readback differs from integer oracle");
                }
                hashComputeWord(actualHash, actual); hashComputeWord(referenceHash, expected);
            }
        }
        runtime.proc<PFN_vkUnmapMemory>("vkUnmapMemory")(runtime.device, objects.readbackMemory); objects.mapped = nullptr;
        require(actualHash == referenceHash && (!round || actualHash != previous), "compute hash repeated or incorrect");
        previous = actualHash;
        std::cout << "VULKAN_COMPUTE_ROUND round=" << round << " verifiedWords=8294400 fnv1a="
            << std::hex << std::setw(16) << std::setfill('0') << actualHash << " referenceFnv1a="
            << std::setw(16) << referenceHash << std::dec << " coherent=" << (coherent ? "true" : "false")
            << " gpuOnlyStorage=true cpuStorageWrites=false" << std::endl;
    }
    require(umdLoaded(), "NVIDIA UMD absent after compute");
    std::cout << "VULKAN_COMPUTE_COMPLETE verified=true width=3840 height=2160 rounds=2 wordsPerRound=8294400 gpuOnlyStorage=true"
        " gpuTransferReadback=true nvidiaUmdLoaded=true window=false cpuStorageWrites=false" << std::endl;
}
}
