// SPDX-License-Identifier: MIT
// Actual Vulkan memory export; no rasterization, presentation or CPU upload.
#pragma once
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <set>

namespace {
struct MemoryFdObjects {
    Runtime& runtime;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<int> descriptors;
    explicit MemoryFdObjects(Runtime& value) : runtime(value) {}
    ~MemoryFdObjects() {
        for (int fd : descriptors) close(fd);
        releaseMemory();
    }
    void releaseMemory() {
        if (buffer) runtime.proc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(runtime.device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        if (memory) runtime.proc<PFN_vkFreeMemory>("vkFreeMemory")(runtime.device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
};

void memoryFdWorkload(Runtime& runtime, VkPhysicalDevice physical) {
    std::cout << "VULKAN_MEMORY_FD_BEGIN bufferBytes=65536 exports=32 cpuWrites=false window=false" << std::endl;
    VkPhysicalDeviceExternalBufferInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    externalInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    runtime.proc<PFN_vkGetPhysicalDeviceExternalBufferProperties>("vkGetPhysicalDeviceExternalBufferProperties")(
        physical, &externalInfo, &externalProperties);
    require((externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
        (externalProperties.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT),
        "opaque FD buffer export unsupported");
    MemoryFdObjects objects(runtime);
    VkExternalMemoryBufferCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; buffer.pNext = &external;
    buffer.size = 65536; buffer.usage = externalInfo.usage; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result("memory-fd-buffer", runtime.proc<PFN_vkCreateBuffer>("vkCreateBuffer")(
        runtime.device, &buffer, nullptr, &objects.buffer));
    VkMemoryRequirements requirements{};
    runtime.proc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(runtime.device, objects.buffer, &requirements);
    require(requirements.size >= buffer.size && requirements.size <= 1024 * 1024, "unexpected shared buffer size");
    VkPhysicalDeviceMemoryProperties properties{};
    runtime.proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &properties);
    require(properties.memoryTypeCount > 0 && properties.memoryTypeCount <= VK_MAX_MEMORY_TYPES, "invalid memory type count");
    std::uint32_t selected = properties.memoryTypeCount;
    for (std::uint32_t index = 0; index < properties.memoryTypeCount && index < VK_MAX_MEMORY_TYPES; ++index) {
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << index)) && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { selected = index; break; }
    }
    require(selected < properties.memoryTypeCount, "GPU-only shared buffer memory absent");
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; dedicated.buffer = objects.buffer;
    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO; exportInfo.pNext = &dedicated;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocate.pNext = &exportInfo;
    allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = selected;
    result("memory-fd-allocate", runtime.proc<PFN_vkAllocateMemory>("vkAllocateMemory")(
        runtime.device, &allocate, nullptr, &objects.memory));
    result("memory-fd-bind", runtime.proc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
        runtime.device, objects.buffer, objects.memory, 0));
    std::set<int> unique;
    struct stat identity{};
    for (unsigned index = 0; index < 32; ++index) {
        VkMemoryGetFdInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR; info.memory = objects.memory;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        result("memory-fd-export", runtime.proc<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(runtime.device, &info, &fd));
        require(fd >= 0 && unique.insert(fd).second, "Vulkan export did not return a fresh valid descriptor");
        objects.descriptors.push_back(fd);
        const auto flags = fcntl(fd, F_GETFD);
        struct stat status{};
        require(flags >= 0 && (flags & FD_CLOEXEC) && fstat(fd, &status) == 0, "export flags/identity");
        if (!index) identity = status;
        require(status.st_dev == identity.st_dev && status.st_ino == identity.st_ino,
            "exported descriptors have different kernel file identity");
    }
    result("memory-fd-idle-before-free", runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(runtime.device));
    objects.releaseMemory();
    for (const int fd : objects.descriptors) {
        struct stat status{};
        require(fcntl(fd, F_GETFD) >= 0 && fstat(fd, &status) == 0 &&
            status.st_dev == identity.st_dev && status.st_ino == identity.st_ino,
            "exported descriptor lost when Vulkan memory was freed");
    }
    require(umdLoaded(), "genuine NVIDIA UMD absent during export");
    std::cout << "VULKAN_MEMORY_FD_COMPLETE verified=true exports=32 unique=true cloexec=true"
        " validAfterMemoryFree=true nvidiaUmdLoaded=true cpuWrites=false window=false" << std::endl;
}
}
