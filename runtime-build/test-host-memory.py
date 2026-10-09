#!/usr/bin/env python3
"""Exercise the actual Windows host allocation helpers with a Vulkan stub."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = (args.source / 'src/venus/vkr_device_memory.c').read_text(encoding='utf-8')

def function(name, result, source=source):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return result + ' ' + source[start:end] + '\n'

harness = r'''
#include <windows.h>
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define WINQ_TRACE_ENTER(...) 0
#define WINQ_TRACE_EXIT(call, ...) do { (void)(call); } while (0)
#define MAX2(a,b) ((a)>(b)?(a):(b))
struct vkr_host_memory { struct vkr_host_memory *next; int fd; void *mapping; };
struct physical { uint64_t min_host_pointer_alignment; VkPhysicalDeviceMemoryProperties memory_properties; };
struct vkr_device {
 struct physical *physical_device;
 struct { struct { VkDevice device; } handle; } base;
 PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT;
 struct vkr_host_memory *host_memories;
};
static HANDLE owned;
static int wrap_failure;
static uint32_t supported = 2;
static VkResult result = VK_SUCCESS;
static int getpagesize(void) { return 4096; }
static int os_wrap_win32_handle(HANDLE h) {
 if (wrap_failure) return -1;
 assert(!owned); owned=h; return 100;
}
static int os_close_fd(int fd) { assert(fd==100 && owned); CloseHandle(owned); owned=NULL; return 0; }
static VKAPI_ATTR VkResult VKAPI_CALL properties(VkDevice device,
 VkExternalMemoryHandleTypeFlagBits type, const void *ptr, VkMemoryHostPointerPropertiesEXT *props) {
 (void)device;
 assert(type==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
 assert(ptr && !((uintptr_t)ptr % 4096));
 props->memoryTypeBits=supported; return result;
}
'''
harness += function('vkr_host_memory_destroy', 'static void')
harness += function('vkr_device_memory_release_host_backings', 'void')
harness += function('vkr_host_memory_create', 'static VkResult')
harness += r'''
struct vkr_buffer { struct { struct { VkBuffer buffer; } handle; } base; VkBuffer plain_buffer,host_buffer; };
struct vkr_image { struct { struct { VkImage image; } handle; } base; VkImage plain_image,host_image; };
'''
for kind in ('buffer', 'image'):
    resource = (args.source / f'src/venus/vkr_{kind}.c').read_text(encoding='utf-8')
    harness += function(f'vkr_{kind}_merge_requirements', 'static void', resource)
    harness += function(f'vkr_{kind}_select_host_memory', 'bool', resource)
harness += r'''
#define TRACE_FUNC() ((void)0)
struct vkr_device_memory { struct vkr_device *device; int base; };
struct vn_dispatch_context { void *data; };
struct vn_command_vkFreeMemory { struct vkr_device_memory *memory; };
static int driver_freed, backing_released, object_removed;
static struct vkr_device_memory *vkr_device_memory_from_handle(struct vkr_device_memory *mem) { return mem; }
static void vkr_device_memory_destroy_driver_handle(void *ctx, struct vn_command_vkFreeMemory *args) {
 (void)ctx; assert(args->memory && !backing_released); driver_freed=1; args->memory=NULL;
}
static void vkr_device_memory_release(struct vkr_device_memory *mem) {
 assert(mem && driver_freed); backing_released=1;
}
static void vkr_device_remove_object(void *ctx, struct vkr_device *dev, int *base) {
 (void)ctx; assert(dev && base && backing_released); object_removed=1;
}
'''
harness += function('vkr_dispatch_vkFreeMemory', 'static void')
harness += r'''
int main(void) {
 struct physical physical={.min_host_pointer_alignment=4096};
 physical.memory_properties.memoryTypeCount=4;
 physical.memory_properties.memoryTypes[0].propertyFlags=1;
 physical.memory_properties.memoryTypes[1].propertyFlags=6;
 physical.memory_properties.memoryTypes[2].propertyFlags=7;
 physical.memory_properties.memoryTypes[3].propertyFlags=14;
 struct vkr_device dev={.physical_device=&physical,.GetMemoryHostPointerPropertiesEXT=properties};
 VkMemoryRequirements plain={4096,256,15},host_req={8192,4096,10};
 vkr_buffer_merge_requirements(&dev,&plain,&host_req);
 assert(plain.size==8192 && plain.alignment==4096 && plain.memoryTypeBits==11);
 plain=(VkMemoryRequirements){4096,256,15};
 vkr_image_merge_requirements(&dev,&plain,&host_req);
 assert(plain.size==8192 && plain.alignment==4096 && plain.memoryTypeBits==11);
 struct vkr_buffer buffer={.plain_buffer=(VkBuffer)(uintptr_t)1,.host_buffer=(VkBuffer)(uintptr_t)2};
 assert(vkr_buffer_select_host_memory(&buffer,false) && buffer.base.handle.buffer==buffer.plain_buffer);
 assert(vkr_buffer_select_host_memory(&buffer,true) && buffer.base.handle.buffer==buffer.host_buffer);
 struct vkr_image image={.plain_image=(VkImage)(uintptr_t)1,.host_image=(VkImage)(uintptr_t)2};
 assert(vkr_image_select_host_memory(&image,false) && image.base.handle.image==image.plain_image);
 assert(vkr_image_select_host_memory(&image,true) && image.base.handle.image==image.host_image);
 image.host_image=VK_NULL_HANDLE;
 assert(!vkr_image_select_host_memory(&image,true));
 VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
 VkMemoryAllocateInfo original={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  .pNext=&flags,.allocationSize=17,.memoryTypeIndex=1};
 VkMemoryAllocateInfo info=original;
 VkImportMemoryHostPointerInfoEXT import;
 struct vkr_host_memory *host=NULL;
 DWORD before,after;
 GetProcessHandleCount(GetCurrentProcess(),&before);
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_SUCCESS);
 assert(host && host->mapping && host->fd==100 && info.allocationSize==4096);
 assert(info.pNext==&import && import.pNext==&flags && import.pHostPointer==host->mapping);
 void *view=MapViewOfFile(owned,FILE_MAP_READ|FILE_MAP_WRITE,0,0,4096);
 assert(view && view!=host->mapping);
 memset(host->mapping,0x5a,4096);
 assert(!memcmp(view,host->mapping,4096));
 dev.host_memories=host;
 vkr_device_memory_release_host_backings(&dev);
 assert(!dev.host_memories && !owned);
 assert(((unsigned char *)view)[4095]==0x5a); // Other owners retain their own view.
 UnmapViewOfFile(view);

 host=NULL; info=original; supported=1;
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 assert(!host && !owned && info.pNext==&flags);
 supported=2; result=VK_ERROR_DEVICE_LOST; info=original;
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_ERROR_DEVICE_LOST);
 assert(!host && !owned);
 result=VK_SUCCESS; wrap_failure=1; info=original;
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_ERROR_OUT_OF_HOST_MEMORY);
 assert(!host && !owned);
 wrap_failure=0; info=original; info.allocationSize=UINT64_MAX;
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_ERROR_OUT_OF_HOST_MEMORY);
 info=original; info.allocationSize=0;
 assert(vkr_host_memory_create(&dev,&info,&host,&import)==VK_ERROR_OUT_OF_HOST_MEMORY);
 struct vn_dispatch_context dispatch={0};
 struct vkr_device_memory memory={.device=&dev};
 struct vn_command_vkFreeMemory free_args={.memory=&memory};
 vkr_dispatch_vkFreeMemory(&dispatch,&free_args);
 assert(driver_freed && backing_released && object_removed);
 GetProcessHandleCount(GetCurrentProcess(),&after);
 assert(before==after);
 puts("PASS: host allocation alignment, shared ownership, incompatible types, overflow and failure cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='venus-host-memory-') as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test.exe'
    src.write_text(harness, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
