// SPDX-License-Identifier: MIT
// Windowless Vulkan bring-up through one explicitly selected Mesa Dozen ICD.
// Device creation alone does not establish shader execution or conformance.
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WDDM_BRIDGE_VULKAN_ICD_PATH
#define WDDM_BRIDGE_VULKAN_ICD_PATH "/opt/wddm-vulkan/libvulkan_dzn.so"
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void result(const char* stage, VkResult value) {
    std::cout << "VULKAN_BRIDGE_RESULT stage=" << stage << " result=" << value << std::endl;
    require(value == VK_SUCCESS, stage);
}
bool umdLoaded() {
    const char* name = std::getenv("WDDM_BRIDGE_LINUX_UMD_NAME");
    if (!name || !*name) return false;
    std::ifstream maps("/proc/self/maps");
    for (std::string row; std::getline(maps, row);)
        if (row.size() > std::char_traits<char>::length(name) &&
            row.compare(row.size() - std::char_traits<char>::length(name),
                        std::char_traits<char>::length(name), name) == 0) return true;
    return false;
}
struct Runtime {
    void* library = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr get = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    ~Runtime() {
        if (device) destroyDevice(device, nullptr);
        if (instance) {
            // A created instance must expose destruction. A broken ICD must
            // never turn an unowned object into a successful test exit.
            if (!destroyInstance) std::abort();
            destroyInstance(instance, nullptr);
        }
        if (library) dlclose(library);
    }
    template<class T> T proc(const char* name) const {
        const auto address = get(instance, name);
        require(address != nullptr, name);
        return reinterpret_cast<T>(address);
    }
};
}

int main(int argc, char** argv) {
    try {
        require(argc == 1 || argc == 2, "usage: vulkan-device-probe [absolute Dozen ICD path]");
        const char* path = argc == 2 ? argv[1] : WDDM_BRIDGE_VULKAN_ICD_PATH;
        require(path[0] == '/', "ICD path must be absolute");
        Runtime runtime;
        runtime.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!runtime.library) {
            std::cerr << "VULKAN_BRIDGE_LOADER_ERROR " << dlerror() << std::endl;
            throw std::runtime_error("explicit ICD dlopen");
        }
        runtime.get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(runtime.library, "vk_icdGetInstanceProcAddr"));
        require(runtime.get != nullptr, "vk_icdGetInstanceProcAddr");
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "QEMU NVIDIA Vulkan bring-up";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create.pApplicationInfo = &application;
        result("create-instance", runtime.proc<PFN_vkCreateInstance>("vkCreateInstance")(
            &create, nullptr, &runtime.instance));
        require(runtime.instance != VK_NULL_HANDLE, "null instance");
        runtime.destroyInstance = runtime.proc<PFN_vkDestroyInstance>("vkDestroyInstance");
        const auto enumerate = runtime.proc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        std::uint32_t count = 0;
        result("count-devices", enumerate(runtime.instance, &count, nullptr));
        require(count > 0 && count <= 16, "physical device count");
        std::vector<VkPhysicalDevice> physical(count);
        result("enumerate-devices", enumerate(runtime.instance, &count, physical.data()));
        VkPhysicalDevice selected = VK_NULL_HANDLE;
        for (std::uint32_t index = 0; index < count; ++index) {
            VkPhysicalDeviceProperties properties{};
            runtime.proc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(
                physical[index], &properties);
            std::cout << "VULKAN_BRIDGE_DEVICE vendor=" << properties.vendorID
                << " device=" << properties.deviceID << " type=" << properties.deviceType
                << " api=" << properties.apiVersion << " name=" << properties.deviceName << std::endl;
            if (properties.vendorID == 0x10de && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                require(!selected, "ambiguous NVIDIA device selection");
                selected = physical[index];
            }
        }
        require(selected != VK_NULL_HANDLE, "no NVIDIA discrete GPU; software fallback forbidden");
        const auto families = runtime.proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
        std::uint32_t familyCount = 0;
        families(selected, &familyCount, nullptr);
        require(familyCount > 0 && familyCount <= 32, "queue family count");
        std::vector<VkQueueFamilyProperties> queues(familyCount);
        families(selected, &familyCount, queues.data());
        std::uint32_t family = familyCount;
        for (std::uint32_t index = 0; index < familyCount; ++index)
            if (queues[index].queueCount && (queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                family = index; break;
            }
        require(family < familyCount, "no compute queue");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        runtime.destroyDevice = runtime.proc<PFN_vkDestroyDevice>("vkDestroyDevice");
        result("create-device", runtime.proc<PFN_vkCreateDevice>("vkCreateDevice")(
            selected, &device, nullptr, &runtime.device));
        require(runtime.device != VK_NULL_HANDLE, "null device");
        VkQueue compute = VK_NULL_HANDLE;
        runtime.proc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(runtime.device, family, 0, &compute);
        require(compute != VK_NULL_HANDLE, "null compute queue");
        result("device-idle", runtime.proc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(runtime.device));
        require(umdLoaded(), "genuine NVIDIA UMD not loaded");
        std::cout << "VULKAN_BRIDGE_DEVICE_READY verified=true vendor=4318 computeQueue=true"
            " nvidiaUmdLoaded=true submissions=0 window=false" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VULKAN_BRIDGE_FAIL " << error.what() << std::endl;
        return 1;
    }
}
