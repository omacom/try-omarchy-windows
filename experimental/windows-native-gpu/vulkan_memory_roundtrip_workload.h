// SPDX-License-Identifier: MIT
// Real GPU writes -> FD export -> creator free -> import -> GPU copy -> readback.
#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <iomanip>
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_GPU_PRESSURE
#include <array>
#endif
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_EXHAUSTION
#include <algorithm>
#include <cerrno>
#include <sys/resource.h>
#endif

namespace {
constexpr VkDeviceSize RoundtripBytes = 65536, StripeBytes = 4096;
struct RoundtripObjects {
    Runtime& runtime;
    VkBuffer creator = VK_NULL_HANDLE, imported = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory creatorMemory = VK_NULL_HANDLE, importedMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int callerOwnedFd = -1;
    void* mapped = nullptr;
    bool pending = false;
    explicit RoundtripObjects(Runtime& value) : runtime(value) {}
    void releaseCreator() {
        if (creator) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(runtime.device, creator, nullptr);
        creator = VK_NULL_HANDLE;
        if (creatorMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(runtime.device, creatorMemory, nullptr);
        creatorMemory = VK_NULL_HANDLE;
    }
    ~RoundtripObjects() {
        const auto device = runtime.device;
        if (pending) runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
        if (mapped) runtime.proc<PFN_vkUnmapMemory>("vkUnmapMemory")(device, readbackMemory);
        if (fence) runtime.proc<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
        if (pool) runtime.proc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
        if (callerOwnedFd >= 0) close(callerOwnedFd); // Only pre-import or failed-import ownership.
        releaseCreator();
        if (imported) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, imported, nullptr);
        if (readback) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, readback, nullptr);
        if (importedMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(device, importedMemory, nullptr);
        if (readbackMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(device, readbackMemory, nullptr);
    }
};

std::uint32_t roundtripPattern(unsigned round, unsigned stripe) {
    return UINT32_C(0x8f24b17d) ^ (UINT32_C(0x01030507) * (stripe + 1)) ^ (UINT32_C(0x13579bdf) * (round + 1));
}
void roundtripHash(std::uint64_t& hash, std::uint32_t word) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (word >> shift) & 255u; hash *= UINT64_C(1099511628211);
    }
}

#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_GPU_PRESSURE
// Real owned GPU allocations, fills and retired fences create pressure. Only
// this diagnostic guest is affected; the host's existing byte guard stays fixed.
void gpuAllocationPressure(Runtime& runtime, VkQueue queue, VkCommandBuffer command,
                           RoundtripObjects& original, VkExternalMemoryBufferCreateInfo& external,
                           VkBufferUsageFlags usage, std::uint32_t gpuType) {
    constexpr VkDeviceSize bytes = 64ull * 1024 * 1024;
    struct PressureObjects {
        Runtime& runtime; bool& pending;
        std::array<VkBuffer, 6> buffers{};
        std::array<VkDeviceMemory, 6> memories{};
        void release(unsigned index) {
            if (buffers[index]) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(runtime.device, buffers[index], nullptr);
            buffers[index] = VK_NULL_HANDLE;
            if (memories[index]) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(runtime.device, memories[index], nullptr);
            memories[index] = VK_NULL_HANDLE;
        }
        ~PressureObjects() {
            if (pending && runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(runtime.device) == VK_SUCCESS) pending = false;
            for (unsigned i = 0; i < buffers.size(); ++i) release(i);
        }
    } owned{runtime, original.pending};
    auto fill = [&](unsigned index) {
        result("pressure-reset-command", runtime.proc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0));
        result("pressure-reset-fence", runtime.proc<PFN_vkResetFences>("vkResetFences")(runtime.device, 1, &original.fence));
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result("pressure-begin-command", runtime.proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin));
        runtime.proc<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(command, owned.buffers[index], 0, bytes, UINT32_C(0x6b170001) ^ index);
        result("pressure-end-command", runtime.proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command));
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        original.pending = true;
        result("pressure-submit", runtime.proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, original.fence));
        result("pressure-wait", runtime.proc<PFN_vkWaitForFences>("vkWaitForFences")(
            runtime.device, 1, &original.fence, VK_TRUE, UINT64_C(30000000000)));
        original.pending = false;
    };
    unsigned refused = static_cast<unsigned>(owned.buffers.size());
    VkMemoryRequirements refusedRequirements{};
    std::cout << "VULKAN_GPU_MEMORY_PRESSURE_BEGIN allocationBytes=" << bytes
        << " maxAttempts=" << owned.buffers.size() << " cpuWrites=false hostGuardChanged=false" << std::endl;
    for (unsigned i = 0; i < owned.buffers.size(); ++i) {
        VkBufferCreateInfo buffer{}; buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.pNext = &external; buffer.size = bytes; buffer.usage = usage; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result("pressure-buffer", runtime.proc<PFN_vkCreateBuffer>("vkCreateBuffer")(runtime.device, &buffer, nullptr, &owned.buffers[i]));
        require(owned.buffers[i] != VK_NULL_HANDLE, "pressure buffer missing");
        VkMemoryRequirements requirements{};
        runtime.proc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(runtime.device, owned.buffers[i], &requirements);
        require(requirements.size == bytes && (requirements.memoryTypeBits & (1u << gpuType)), "unexpected pressure buffer requirements");
        VkMemoryDedicatedAllocateInfo dedicated{}; dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.buffer = owned.buffers[i];
        VkExportMemoryAllocateInfo exported{}; exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exported.pNext = &dedicated; exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocate{}; allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &exported; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = gpuType;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const auto status = runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(runtime.device, &allocate, nullptr, &memory);
        if (status != VK_SUCCESS) {
            require((status == VK_ERROR_OUT_OF_DEVICE_MEMORY || status == VK_ERROR_OUT_OF_HOST_MEMORY) &&
                memory == VK_NULL_HANDLE && i && original.callerOwnedFd >= 0 && fcntl(original.callerOwnedFd, F_GETFD) >= 0,
                "pressure did not return a recoverable allocation refusal");
            refused = i; refusedRequirements = requirements;
            std::cout << "VULKAN_GPU_MEMORY_PRESSURE_REFUSED heldBuffers=" << i << " result=" << status
                << " allocationBytes=" << bytes << " outputNull=true callerFdRetained=true" << std::endl;
            break;
        }
        require(memory != VK_NULL_HANDLE, "pressure memory missing"); owned.memories[i] = memory;
        result("pressure-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(runtime.device, owned.buffers[i], memory, 0));
        fill(i);
        std::cout << "VULKAN_GPU_MEMORY_PRESSURE_HELD index=" << i << " allocationBytes=" << bytes << " gpuFillRetired=true" << std::endl;
    }
    require(refused < owned.buffers.size(), "bounded GPU pressure did not reach the existing guard");
    for (unsigned i = 0; i < refused; ++i) owned.release(i);
    result("pressure-device-idle", runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(runtime.device));
    VkMemoryDedicatedAllocateInfo dedicated{}; dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = owned.buffers[refused];
    VkExportMemoryAllocateInfo exported{}; exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exported.pNext = &dedicated; exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo retry{}; retry.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    retry.pNext = &exported; retry.allocationSize = refusedRequirements.size; retry.memoryTypeIndex = gpuType;
    result("pressure-retry-memory", runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
        runtime.device, &retry, nullptr, &owned.memories[refused]));
    require(owned.memories[refused] != VK_NULL_HANDLE, "pressure retry memory missing");
    result("pressure-retry-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
        runtime.device, owned.buffers[refused], owned.memories[refused], 0));
    fill(refused);
    require(original.callerOwnedFd >= 0 && fcntl(original.callerOwnedFd, F_GETFD) >= 0, "GPU pressure lost original caller FD");
    owned.release(refused);
    std::cout << "VULKAN_GPU_MEMORY_PRESSURE_RECOVERED freedBuffers=" << refused
        << " retrySameBuffer=true retrySameSize=true retrySameMemoryType=true retryGpuFillRetired=true callerFdRetained=true" << std::endl;
}
#endif

void memoryRoundtripWorkload(Runtime& runtime, VkPhysicalDevice physical, std::uint32_t family, VkQueue queue) {
    std::cout << "VULKAN_MEMORY_ROUNDTRIP_BEGIN bufferBytes=65536 stripes=16 rounds=2 cpuWrites=false window=false" << std::endl;
    VkPhysicalDeviceExternalBufferInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    externalInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    runtime.proc<PFN_vkGetPhysicalDeviceExternalBufferProperties>("vkGetPhysicalDeviceExternalBufferProperties")(
        physical, &externalInfo, &externalProperties);
    const auto& capabilities = externalProperties.externalMemoryProperties;
    constexpr VkExternalMemoryFeatureFlags needed = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    require((capabilities.externalMemoryFeatures & needed) == needed &&
        (capabilities.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT), "opaque FD import/export unsupported");
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    runtime.proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memoryProperties);
    require(memoryProperties.memoryTypeCount > 0 && memoryProperties.memoryTypeCount <= VK_MAX_MEMORY_TYPES, "invalid memory types");
    std::uint64_t previous = 0;
    for (unsigned round = 0; round < 2; ++round) {
        RoundtripObjects objects(runtime);
        VkExternalMemoryBufferCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        auto createBuffer = [&](VkBuffer& handle, bool shared, const char* stage) {
            VkBufferCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            create.pNext = shared ? &external : nullptr; create.size = RoundtripBytes;
            create.usage = externalInfo.usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            result(stage, runtime.proc<PFN_vkCreateBuffer>("vkCreateBuffer")(runtime.device, &create, nullptr, &handle));
            VkMemoryRequirements requirements{};
            runtime.proc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(runtime.device, handle, &requirements);
            require(requirements.size >= RoundtripBytes && requirements.size <= 1024 * 1024, "unexpected buffer size");
            return requirements;
        };
        auto selectType = [&](VkMemoryRequirements requirements, bool gpuOnly) {
            for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
                const auto flags = memoryProperties.memoryTypes[index].propertyFlags;
                const bool eligible = gpuOnly ? ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) :
                    ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
                if ((requirements.memoryTypeBits & (1u << index)) && eligible) return index;
            }
            throw std::runtime_error("GPU-only/coherent readback type absent");
        };
        auto requirements = createBuffer(objects.creator, true, "roundtrip-creator-buffer");
        const auto gpuType = selectType(requirements, true);
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; dedicated.buffer = objects.creator;
        VkExportMemoryAllocateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO; exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocate{}; allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &exportInfo; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = gpuType;
        result("roundtrip-creator-memory", runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
            runtime.device, &allocate, nullptr, &objects.creatorMemory));
        result("roundtrip-creator-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
            runtime.device, objects.creator, objects.creatorMemory, 0));
        VkCommandPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = family; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        result("roundtrip-command-pool", runtime.proc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
            runtime.device, &pool, nullptr, &objects.pool));
        VkCommandBufferAllocateInfo commandInfo{}; commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = objects.pool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        result("roundtrip-command-buffer", runtime.proc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
            runtime.device, &commandInfo, &command));
        VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result("roundtrip-fence", runtime.proc<PFN_vkCreateFence>("vkCreateFence")(runtime.device, &fence, nullptr, &objects.fence));
        auto begin = [&] {
            VkCommandBufferBeginInfo info{}; info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result("roundtrip-begin-command", runtime.proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &info));
        };
        auto submit = [&] {
            result("roundtrip-end-command", runtime.proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command));
            VkSubmitInfo submitInfo{}; submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1; submitInfo.pCommandBuffers = &command;
            objects.pending = true;
            result("roundtrip-submit", runtime.proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submitInfo, objects.fence));
            result("roundtrip-wait", runtime.proc<PFN_vkWaitForFences>("vkWaitForFences")(
                runtime.device, 1, &objects.fence, VK_TRUE, UINT64_C(30000000000)));
            objects.pending = false;
        };
        auto ownership = [&](VkBuffer buffer, bool release) {
            VkBufferMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = release ? VkAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT) : VkAccessFlags(0);
            barrier.dstAccessMask = release ? VkAccessFlags(0) : VkAccessFlags(VK_ACCESS_TRANSFER_READ_BIT);
            barrier.srcQueueFamilyIndex = release ? family : VK_QUEUE_FAMILY_EXTERNAL;
            barrier.dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : family;
            barrier.buffer = buffer; barrier.size = RoundtripBytes;
            runtime.proc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,
                release ? VkPipelineStageFlags(VK_PIPELINE_STAGE_TRANSFER_BIT) : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                release ? VkPipelineStageFlags(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) : VkPipelineStageFlags(VK_PIPELINE_STAGE_TRANSFER_BIT),
                0, 0, nullptr, 1, &barrier, 0, nullptr);
        };
        begin();
        for (unsigned stripe = 0; stripe < 16; ++stripe) {
            runtime.proc<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(command, objects.creator, StripeBytes * stripe, StripeBytes,
                roundtripPattern(round, stripe));
        }
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_CORRUPT
        VkMemoryBarrier overwrite{}; overwrite.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        overwrite.srcAccessMask = overwrite.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        runtime.proc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &overwrite, 0, nullptr, 0, nullptr);
        runtime.proc<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(command, objects.creator, RoundtripBytes - 4, 4, 0);
#endif
        ownership(objects.creator, true);
        submit();
        VkMemoryGetFdInfoKHR get{}; get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        get.memory = objects.creatorMemory; get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        result("roundtrip-export", runtime.proc<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(
            runtime.device, &get, &objects.callerOwnedFd));
        require(objects.callerOwnedFd >= 0 && (fcntl(objects.callerOwnedFd, F_GETFD) & FD_CLOEXEC), "invalid exported descriptor");
        const auto originalRequirements = requirements;
        objects.releaseCreator();
        std::cout << "VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED round=" << round << " beforeImport=true gpuFillRetired=true" << std::endl;
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_GPU_PRESSURE
        if (!round) gpuAllocationPressure(runtime, queue, command, objects, external, externalInfo.usage, gpuType);
#endif
        requirements = createBuffer(objects.imported, true, "roundtrip-import-buffer");
        require(requirements.size == originalRequirements.size && (requirements.memoryTypeBits & (1u << gpuType)), "incompatible import buffer");
        dedicated.buffer = objects.imported;
        VkImportMemoryFdInfoKHR importInfo{}; importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        importInfo.pNext = &dedicated; importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_IMPORT_ZERO
        // Move a still caller-owned FD; successful Vulkan import consumes it.
        const int originalFd = objects.callerOwnedFd;
        if (originalFd != STDIN_FILENO) {
            require(dup2(originalFd, STDIN_FILENO) == STDIN_FILENO, "move import descriptor to zero");
            objects.callerOwnedFd = STDIN_FILENO;
            require(close(originalFd) == 0, "close moved caller descriptor");
        }
        std::cout << "VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD round=" << round
            << " descriptorZero=true movedWhileCallerOwned=true" << std::endl;
#endif
        importInfo.fd = objects.callerOwnedFd;
        allocate.pNext = &importInfo;
#ifdef WDDM_BRIDGE_FD_ROUNDTRIP_EXHAUSTION
        // Only this guest process's descriptor limit changes. A failed import
        // retains caller ownership; restore the limit before retrying it.
        struct rlimit previousLimit{};
        require(getrlimit(RLIMIT_NOFILE, &previousLimit) == 0, "query guest descriptor limit");
        auto boundedLimit = previousLimit;
        boundedLimit.rlim_cur = std::min<rlim_t>(previousLimit.rlim_cur, 64);
        require(boundedLimit.rlim_cur > 8 && setrlimit(RLIMIT_NOFILE, &boundedLimit) == 0, "bound guest descriptor limit");
        std::vector<int> occupied;
        VkDeviceMemory rejectedMemory = VK_NULL_HANDLE;
        VkResult rejected = VK_ERROR_UNKNOWN;
        try {
            for (;;) {
                const int fd = fcntl(objects.callerOwnedFd, F_DUPFD_CLOEXEC, 3);
                if (fd < 0) { require(errno == EMFILE, "exhaust guest descriptors"); break; }
                try { occupied.push_back(fd); } catch (...) { close(fd); throw; }
            }
            rejected = runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
                runtime.device, &allocate, nullptr, &rejectedMemory);
        } catch (...) {
            for (const int fd : occupied) close(fd);
            setrlimit(RLIMIT_NOFILE, &previousLimit);
            throw;
        }
        for (const int fd : occupied) close(fd);
        require(setrlimit(RLIMIT_NOFILE, &previousLimit) == 0, "restore guest descriptor limit");
        if (rejected == VK_SUCCESS) {
            objects.callerOwnedFd = -1;
            if (rejectedMemory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(runtime.device, rejectedMemory, nullptr);
            throw std::runtime_error("import unexpectedly succeeded under descriptor exhaustion");
        }
        const bool callerRetained = fcntl(objects.callerOwnedFd, F_GETFD) >= 0;
        if (!callerRetained) objects.callerOwnedFd = -1;
        require(rejected == VK_ERROR_OUT_OF_HOST_MEMORY && rejectedMemory == VK_NULL_HANDLE && callerRetained,
            "failed import lost caller descriptor or changed output");
        std::cout << "VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE round=" << round
            << " result=-1 callerRetained=true outputNull=true limitRestored=true retry=true" << std::endl;
#endif
        result("roundtrip-import-memory", runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
            runtime.device, &allocate, nullptr, &objects.importedMemory));
        objects.callerOwnedFd = -1; // Ownership transferred. Never inspect/close the consumed descriptor.
        result("roundtrip-import-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
            runtime.device, objects.imported, objects.importedMemory, 0));
        requirements = createBuffer(objects.readback, false, "roundtrip-readback-buffer");
        allocate.pNext = nullptr; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = selectType(requirements, false);
        result("roundtrip-readback-memory", runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
            runtime.device, &allocate, nullptr, &objects.readbackMemory));
        result("roundtrip-readback-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
            runtime.device, objects.readback, objects.readbackMemory, 0));
        result("roundtrip-reset-command", runtime.proc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0));
        result("roundtrip-reset-fence", runtime.proc<PFN_vkResetFences>("vkResetFences")(runtime.device, 1, &objects.fence));
        begin(); ownership(objects.imported, false);
        VkBufferCopy copy{0, 0, RoundtripBytes};
        runtime.proc<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(command, objects.imported, objects.readback, 1, &copy);
        VkMemoryBarrier memory{}; memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        runtime.proc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        submit();
        result("roundtrip-map-readback", runtime.proc<PFN_vkMapMemory>("vkMapMemory")(
            runtime.device, objects.readbackMemory, 0, VK_WHOLE_SIZE, 0, &objects.mapped));
        require(objects.mapped != nullptr, "null readback mapping");
        const auto words = static_cast<const std::uint32_t*>(objects.mapped);
        std::uint64_t hash = UINT64_C(14695981039346656037), expectedHash = hash;
        for (unsigned index = 0; index < RoundtripBytes / 4; ++index) {
            const auto actual = words[index], expected = roundtripPattern(round, index / (StripeBytes / 4));
            if (actual != expected) {
                std::cerr << "VULKAN_MEMORY_ROUNDTRIP_MISMATCH round=" << round << " word=" << index
                    << " actual=" << actual << " expected=" << expected << std::endl;
                throw std::runtime_error("imported GPU payload differs from integer oracle");
            }
            roundtripHash(hash, actual); roundtripHash(expectedHash, expected);
        }
        runtime.proc<PFN_vkUnmapMemory>("vkUnmapMemory")(runtime.device, objects.readbackMemory); objects.mapped = nullptr;
        require(hash == expectedHash && (!round || hash != previous) && umdLoaded(), "repeated/incorrect hash or NVIDIA UMD absent");
        previous = hash;
        std::cout << "VULKAN_MEMORY_ROUNDTRIP_ROUND round=" << round << " verifiedWords=16384 fnv1a=" << std::hex
            << std::setw(16) << std::setfill('0') << hash << " referenceFnv1a=" << std::setw(16) << expectedHash << std::dec
            << " gpuOnlyStorage=true creatorFreed=true importOwnsFd=true gpuTransferReadback=true cpuWrites=false" << std::endl;
    }
    std::cout << "VULKAN_MEMORY_ROUNDTRIP_COMPLETE verified=true rounds=2 wordsPerRound=16384 gpuOnlyStorage=true"
        " creatorFreedBeforeImport=true gpuTransferReadback=true nvidiaUmdLoaded=true window=false cpuWrites=false" << std::endl;
}
}
