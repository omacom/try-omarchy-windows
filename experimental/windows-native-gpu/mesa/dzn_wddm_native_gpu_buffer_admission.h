/* SPDX-License-Identifier: MIT
 * Opt-in, same-thread declaration of a dedicated external GPU-only buffer.
 * Public descriptions only: no opaque D3D12/KMT bytes are inspected here.
 */
#ifndef DZN_WDDM_NATIVE_GPU_BUFFER_ADMISSION_H
#define DZN_WDDM_NATIVE_GPU_BUFFER_ADMISSION_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>

struct dzn_wddm_buffer_scope {
   int (*cancel)(void);
};

static inline VkResult
dzn_wddm_buffer_begin(struct dzn_wddm_buffer_scope *scope,
                      const D3D12_RESOURCE_DESC *desc,
                      const D3D12_HEAP_PROPERTIES *heap,
                      D3D12_HEAP_FLAGS heap_flags,
                      VkExternalMemoryHandleTypeFlags export_flags)
{
   scope->cancel = NULL;
   const char *enabled = getenv("DZN_WDDM_NATIVE_GPU_BUFFERS");
   if (!enabled || strcmp(enabled, "true"))
      return VK_SUCCESS;
   if (!(export_flags & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
      return VK_SUCCESS;
   /* Dozen uses CUSTOM properties even for discrete GPU-only memory. */
   const bool gpu_only = heap->Type == D3D12_HEAP_TYPE_DEFAULT ||
      (heap->Type == D3D12_HEAP_TYPE_CUSTOM &&
       heap->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
       heap->MemoryPoolPreference == D3D12_MEMORY_POOL_L1);
   if (!gpu_only)
      return VK_SUCCESS;
   if (heap_flags != D3D12_HEAP_FLAG_SHARED ||
       heap->CreationNodeMask > 1 || heap->VisibleNodeMask > 1 ||
       desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
       (desc->Alignment && desc->Alignment != 65536) ||
       !desc->Width || desc->Width > UINT64_C(67108864) ||
       desc->Height != 1 || desc->DepthOrArraySize != 1 || desc->MipLevels != 1 ||
       desc->Format != DXGI_FORMAT_UNKNOWN || desc->SampleDesc.Count != 1 ||
       desc->SampleDesc.Quality || desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
       (desc->Flags != D3D12_RESOURCE_FLAG_NONE && desc->Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   int (*supported)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "wddm_bridge_native_gpu_buffer_supported");
   int (*prepare)(uint64_t, uint32_t) = (int (*)(uint64_t, uint32_t))dlsym(RTLD_DEFAULT, "wddm_bridge_prepare_native_gpu_buffer");
   int (*cancel)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "wddm_bridge_cancel_native_gpu_buffer");
   if (!supported || !prepare || !cancel || supported() != 1)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   const int prepared = prepare(desc->Width, (uint32_t)desc->Flags);
   if (prepared != 0)
      return prepared == -1 && errno == ENOMEM ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_INVALID_EXTERNAL_HANDLE;
   scope->cancel = cancel;
   return VK_SUCCESS;
}

static inline bool
dzn_wddm_buffer_finish(struct dzn_wddm_buffer_scope *scope)
{
   int (*cancel)(void) = scope->cancel;
   scope->cancel = NULL;
   return !cancel || cancel() == 0;
}
#endif
