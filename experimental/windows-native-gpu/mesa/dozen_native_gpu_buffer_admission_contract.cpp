// SPDX-License-Identifier: MIT
// Exercise the actual ICD helper with SDK public types and mock bridge exports.
// No GPU or private runtime is loaded by these contract controls.
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static void* mockSymbol(void*, const char*);
#define dlsym mockSymbol
#include "dzn_wddm_native_gpu_buffer_admission.h"
#undef dlsym
static unsigned queries = 0, prepares = 0, cancels = 0;
static int supportedResult = 1, prepareResult = 0, cancelResult = 0;
static bool missing = false;
static int prepareErrno = 0;
static int supported() { ++queries; return supportedResult; }
static int prepare(std::uint64_t bytes, std::uint32_t flags) {
    if (bytes != 65536 || (flags != 0 && flags != 4)) std::abort();
    ++prepares; if (prepareResult) errno = prepareErrno; return prepareResult;
}
static int cancel() { ++cancels; return cancelResult; }
static void* mockSymbol(void*, const char* name) {
    if (missing) return nullptr;
    if (!std::strcmp(name, "wddm_bridge_native_gpu_buffer_supported")) return reinterpret_cast<void*>(&supported);
    if (!std::strcmp(name, "wddm_bridge_prepare_native_gpu_buffer")) return reinterpret_cast<void*>(&prepare);
    if (!std::strcmp(name, "wddm_bridge_cancel_native_gpu_buffer")) return reinterpret_cast<void*>(&cancel);
    std::abort();
}
static unsigned checks = 0;
static void require(bool value) { ++checks; if (!value) { std::fprintf(stderr, "Native buffer contract failed at check %u\n", checks); std::abort(); } }
int main() {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 65536; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES h{}; h.Type = D3D12_HEAP_TYPE_CUSTOM;
    h.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; h.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
    dzn_wddm_buffer_scope s{};
    const auto beginResult = [&]() { return dzn_wddm_buffer_begin(&s, &d, &h, D3D12_HEAP_FLAG_SHARED, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT); };
    const auto begin = [&]() { return beginResult() == VK_SUCCESS; };
    unsetenv("DZN_WDDM_NATIVE_GPU_BUFFERS"); require(begin() && !s.cancel && !queries);
    setenv("DZN_WDDM_NATIVE_GPU_BUFFERS", "true", 1);
    for (const auto flags : {D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}) {
        d.Flags = flags; require(begin() && s.cancel && dzn_wddm_buffer_finish(&s) && !s.cancel);
        const auto before = cancels; require(dzn_wddm_buffer_finish(&s) && cancels == before);
    }
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    require(queries == 2 && prepares == 2 && cancels == 2);
    for (unsigned n = 0; n < 16; ++n) {
        auto bad = d;
        switch (n) {
        case 0: bad.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
        case 1: bad.Alignment = 4096; break;
        case 2: bad.Width = 0; break;
        case 3: bad.Width = 67108865; break;
        case 4: bad.Width = UINT64_MAX; break;
        case 5: bad.Height = 2; break;
        case 6: bad.DepthOrArraySize = 2; break;
        case 7: bad.MipLevels = 2; break;
        case 8: bad.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case 9: bad.SampleDesc.Count = 2; break;
        case 10: bad.SampleDesc.Quality = 1; break;
        case 11: bad.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; break;
        case 12: bad.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; break;
        case 13: h.CreationNodeMask = 2; break;
        case 14: h.VisibleNodeMask = 2; break;
        case 15: bad.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE; break;
        }
        require(dzn_wddm_buffer_begin(&s, &bad, &h, D3D12_HEAP_FLAG_SHARED, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == VK_ERROR_INVALID_EXTERNAL_HANDLE && !s.cancel);
        h.CreationNodeMask = h.VisibleNodeMask = 0;
    }
    require(queries == 2 && prepares == 2);
    require(dzn_wddm_buffer_begin(&s, &d, &h, D3D12_HEAP_FLAG_NONE, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    require(dzn_wddm_buffer_begin(&s, &d, &h, D3D12_HEAP_FLAG_NONE, 0) == VK_SUCCESS && !s.cancel);
    h.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; require(begin() && !s.cancel && queries == 2);
    h.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    supportedResult = 0; require(!begin() && !s.cancel && prepares == 2);
    supportedResult = -1; require(!begin() && !s.cancel && prepares == 2);
    supportedResult = 1; missing = true; require(!begin() && !s.cancel); missing = false;
    prepareResult = -1; prepareErrno = ENOMEM;
    require(beginResult() == VK_ERROR_OUT_OF_DEVICE_MEMORY && !s.cancel && cancels == 2);
    prepareErrno = EIO; require(beginResult() == VK_ERROR_INVALID_EXTERNAL_HANDLE && !s.cancel);
    prepareResult = 1; prepareErrno = ENOMEM; require(beginResult() == VK_ERROR_INVALID_EXTERNAL_HANDLE && !s.cancel);
    prepareResult = 0; prepareErrno = 0;
    require(begin()); cancelResult = -1; require(!dzn_wddm_buffer_finish(&s) && !s.cancel); cancelResult = 0;
    require(dzn_wddm_buffer_finish(&s));
    h.Type = D3D12_HEAP_TYPE_DEFAULT; require(begin() && dzn_wddm_buffer_finish(&s));
    std::printf("DOZEN_NATIVE_GPU_BUFFER_ADMISSION_CONTRACT_PASS checks=%u optIn=true customGpuHeap=true oneShotCancellation=true failClosed=true gpu=false\n", checks);
}
