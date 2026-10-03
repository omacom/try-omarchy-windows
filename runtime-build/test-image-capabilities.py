#!/usr/bin/env python3
"""Compile the patched Windows image dispatch with mocked Vulkan entry points.

Runs as a console test on Linux or MSYS2, without a Vulkan loader, GPU or GUI.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = (args.source / 'src/venus/vkr_image.c').read_text(encoding='utf-8')


def function(name, result):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return result + ' ' + source[start:end] + '\n'


harness = r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#define _WIN32
#endif
#define UNUSED
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define HANDLE(type, n) ((type)(uintptr_t)(n))
struct vkr_physical_device {
 struct { union { VkPhysicalDevice physical_device; } handle; } base;
 struct { PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2; } proc_table;
 VkPhysicalDeviceMemoryProperties memory_properties;
 bool host_external_memory_host, dma_buf_shim_active;
};
struct vn_device_proc_table {
 PFN_vkCreateImage CreateImage;
 PFN_vkDestroyImage DestroyImage;
 PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
 PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2;
 PFN_vkGetDeviceImageMemoryRequirements GetDeviceImageMemoryRequirements;
 PFN_vkBindImageMemory BindImageMemory;
 PFN_vkBindImageMemory2 BindImageMemory2;
};
struct vkr_device {
 struct { union { VkDevice device; } handle; } base;
 struct vkr_physical_device *physical_device;
 struct vn_device_proc_table proc_table;
};
struct vkr_image {
 struct { union { VkImage image; } handle; } base;
 VkImage plain_image, host_image;
 VkMemoryRequirements combined_requirements; /* Also permits testing the old patch. */
 bool dma_buf_shim_linear;
};
struct vkr_device_memory { void *host_memory; VkDeviceMemory native; };
struct vn_dispatch_context { void *data; };
struct vn_command_vkCreateImage { VkDevice device; const VkImageCreateInfo *pCreateInfo; VkResult ret; };
struct vn_command_vkGetImageMemoryRequirements { VkDevice device; VkImage image; VkMemoryRequirements *pMemoryRequirements; };
struct vn_command_vkGetImageMemoryRequirements2 { VkDevice device; const VkImageMemoryRequirementsInfo2 *pInfo; VkMemoryRequirements2 *pMemoryRequirements; };
struct vn_command_vkGetDeviceImageMemoryRequirements { VkDevice device; const VkDeviceImageMemoryRequirements *pInfo; VkMemoryRequirements2 *pMemoryRequirements; };
struct vn_command_vkBindImageMemory { VkDevice device; VkImage image; VkDeviceMemory memory; VkDeviceSize memoryOffset; VkResult ret; };
struct vn_command_vkBindImageMemory2 { VkDevice device; uint32_t bindInfoCount; const VkBindImageMemoryInfo *pBindInfos; VkResult ret; };
static void *vkr_find_struct(const void *chain, VkStructureType type) {
 for (const VkBaseInStructure *p=chain; p; p=p->pNext)
  if (p->sType==type) return (void *)p;
 return NULL;
}
static struct vkr_device *vkr_device_from_handle(VkDevice h) { return (void *)h; }
static struct vkr_image *vkr_image_from_handle(VkImage h) { return (void *)h; }
static struct vkr_device_memory *vkr_device_memory_from_handle(VkDeviceMemory h) { return (void *)h; }
static struct vkr_image created;
static VkImageCreateInfo expected;
static VkResult capability_result, host_create_result;
static VkExternalMemoryFeatureFlags features;
static VkExternalMemoryHandleTypeFlags compatible;
static VkImageFormatProperties limits;
static unsigned capability_calls, plain_creates, host_creates, legacy_queries, queries2, device_queries, binds, destroys;
static VkImageCreateFlags created_flags;
static const void *expected_query_chain;
static VkImageAspectFlagBits expected_plane;
static bool fail_plain;
static VkMemoryRequirements requirements(bool host, VkImageAspectFlagBits plane) {
 if (plane==VK_IMAGE_ASPECT_PLANE_0_BIT) return (VkMemoryRequirements){4096,256,3};
 if (plane==VK_IMAGE_ASPECT_PLANE_1_BIT) return (VkMemoryRequirements){2048,128,9};
 if (plane==VK_IMAGE_ASPECT_PLANE_2_BIT) return (VkMemoryRequirements){1024,64,5};
 return host ? (VkMemoryRequirements){8192,4096,6} : (VkMemoryRequirements){4096,256,11};
}
static VKAPI_ATTR VkResult VKAPI_CALL capability(VkPhysicalDevice physical,
 const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
 assert(physical==HANDLE(VkPhysicalDevice,20)); ++capability_calls;
 assert(info->format==expected.format && info->type==expected.imageType);
 assert(info->tiling==expected.tiling && info->usage==expected.usage && info->flags==expected.flags);
 const VkPhysicalDeviceExternalImageFormatInfo *ext=info->pNext;
 assert(ext && ext->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
 assert(ext->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
 assert(ext->pNext==expected.pNext);
 VkExternalImageFormatProperties *props=out->pNext;
 assert(props && props->sType==VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
 props->externalMemoryProperties=(VkExternalMemoryProperties){features,0,compatible};
 out->imageFormatProperties=limits;
 return capability_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice device, const VkImageCreateInfo *info,
 const VkAllocationCallbacks *alloc, VkImage *image) {
 assert(device==HANDLE(VkDevice,10) && !alloc);
 created_flags=info->flags;
 const VkExternalMemoryImageCreateInfo *ext=vkr_find_struct(info->pNext,VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
 if (ext && ext->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
  ++host_creates; assert(capability_calls && capability_result==VK_SUCCESS);
  assert(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
  if (host_create_result!=VK_SUCCESS) { *image=HANDLE(VkImage,99); return host_create_result; }
  *image=HANDLE(VkImage,2);
 } else { ++plain_creates; *image=HANDLE(VkImage,1); }
 return VK_SUCCESS;
}
static struct vkr_image *vkr_image_create_and_add(void *ctx, struct vn_command_vkCreateImage *args) {
 (void)ctx;
 if (fail_plain) { args->ret=VK_ERROR_OUT_OF_DEVICE_MEMORY; return NULL; }
 args->device=vkr_device_from_handle(args->device)->base.handle.device;
 args->ret=create(args->device,args->pCreateInfo,NULL,&created.base.handle.image);
 return args->ret==VK_SUCCESS ? &created : NULL;
}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice dev, VkImage image, const VkAllocationCallbacks *alloc) {
 assert(dev==HANDLE(VkDevice,10) && !alloc && image==HANDLE(VkImage,2)); ++destroys;
}
static VKAPI_ATTR void VKAPI_CALL legacy(VkDevice dev, VkImage image, VkMemoryRequirements *out) {
 assert(dev==HANDLE(VkDevice,10));
 assert(!(created_flags & VK_IMAGE_CREATE_DISJOINT_BIT)); ++legacy_queries;
 *out=requirements(image==HANDLE(VkImage,2),0);
}
static void dedicated(VkMemoryRequirements2 *out, bool host) {
 VkMemoryDedicatedRequirements *d=vkr_find_struct(out->pNext,VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
 if (d) { d->prefersDedicatedAllocation=!host; d->requiresDedicatedAllocation=host; }
}
static VKAPI_ATTR void VKAPI_CALL requirements2(VkDevice dev,
 const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out) {
 assert(dev==HANDLE(VkDevice,10)); ++queries2;
 assert(info->pNext==expected_query_chain);
 const VkImagePlaneMemoryRequirementsInfo *plane=vkr_find_struct(info->pNext,VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO);
 assert((plane ? plane->planeAspect : 0)==expected_plane);
 assert(info->image==HANDLE(VkImage,1) || info->image==HANDLE(VkImage,2));
 bool host=info->image==HANDLE(VkImage,2);
 out->memoryRequirements=requirements(host,expected_plane); dedicated(out,host);
}
static VKAPI_ATTR void VKAPI_CALL device_requirements(VkDevice dev,
 const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out) {
 assert(dev==HANDLE(VkDevice,10)); ++device_queries;
 assert(info->planeAspect==expected_plane);
 bool host=vkr_find_struct(info->pCreateInfo->pNext,VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)!=NULL;
 if (host) assert(capability_calls && capability_result==VK_SUCCESS);
 out->memoryRequirements=requirements(host,info->planeAspect); dedicated(out,host);
}
static VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice dev, VkImage image, VkDeviceMemory mem, VkDeviceSize offset) {
 assert(dev==HANDLE(VkDevice,10) && !offset);
 assert((image==HANDLE(VkImage,1) && mem==HANDLE(VkDeviceMemory,1)) ||
        (image==HANDLE(VkImage,2) && mem==HANDLE(VkDeviceMemory,2)));
 ++binds; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL bind2(VkDevice dev, uint32_t count, const VkBindImageMemoryInfo *infos) {
 for (uint32_t i=0;i<count;i++) {
  const VkBindImagePlaneMemoryInfo *plane=vkr_find_struct(infos[i].pNext,VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO);
  assert(plane && plane->planeAspect==expected_plane);
  assert(bind(dev,infos[i].image,infos[i].memory,infos[i].memoryOffset)==VK_SUCCESS);
 }
 return VK_SUCCESS;
}
#define REPLACE_DEVICE(args) ((args)->device=vkr_device_from_handle((args)->device)->base.handle.device)
static void vn_replace_vkGetImageMemoryRequirements_args_handle(struct vn_command_vkGetImageMemoryRequirements *args) {
 REPLACE_DEVICE(args); args->image=vkr_image_from_handle(args->image)->base.handle.image;
}
static void vn_replace_vkGetImageMemoryRequirements2_args_handle(struct vn_command_vkGetImageMemoryRequirements2 *args) {
 REPLACE_DEVICE(args); VkImageMemoryRequirementsInfo2 *info=(void *)args->pInfo;
 info->image=vkr_image_from_handle(info->image)->base.handle.image;
}
static void vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(struct vn_command_vkGetDeviceImageMemoryRequirements *args) { REPLACE_DEVICE(args); }
static void vn_replace_vkBindImageMemory_args_handle(struct vn_command_vkBindImageMemory *args) {
 REPLACE_DEVICE(args); args->image=vkr_image_from_handle(args->image)->base.handle.image;
 args->memory=vkr_device_memory_from_handle(args->memory)->native;
}
static void vn_replace_vkBindImageMemory2_args_handle(struct vn_command_vkBindImageMemory2 *args) {
 REPLACE_DEVICE(args);
 for (uint32_t i=0;i<args->bindInfoCount;i++) {
  VkBindImageMemoryInfo *info=(void *)&args->pBindInfos[i];
  info->image=vkr_image_from_handle(info->image)->base.handle.image;
  info->memory=vkr_device_memory_from_handle(info->memory)->native;
 }
}
'''
# The optional helpers allow running this fixture against the previous patch to
# demonstrate that the dispatch assertions, rather than name checks, catch it.
for name, result in (
    ('vkr_image_needs_host_backing', 'static bool'),
    ('vkr_image_supports_host_backing', 'static bool'),
    ('vkr_image_merge_requirements', 'static void'),
    ('vkr_image_select_host_memory', 'bool'),
    ('vkr_image_release_alternate', 'void'),
    ('vkr_image_prepare_host_create_info', 'static void'),
    ('vkr_dispatch_vkCreateImage', 'static void'),
    ('vkr_dispatch_vkGetImageMemoryRequirements', 'static void'),
    ('vkr_dispatch_vkGetImageMemoryRequirements2', 'static void'),
    ('vkr_dispatch_vkGetDeviceImageMemoryRequirements', 'static void'),
    ('vkr_dispatch_vkBindImageMemory', 'static void'),
    ('vkr_dispatch_vkBindImageMemory2', 'static void'),
):
    if name.startswith(('vkr_image_needs_', 'vkr_image_supports_')) and '\n' + name + '(' not in source:
        continue
    harness += function(name, result)
harness += r'''
static struct vkr_physical_device physical;
static struct vkr_device dev;
static struct vn_dispatch_context dispatch;
static void reset(void) {
 memset(&created,0,sizeof(created));
 expected=(VkImageCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
  .format=VK_FORMAT_R8G8B8A8_UNORM,.imageType=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,
  .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,.extent={64,32,1},.mipLevels=1,.arrayLayers=1,
  .samples=VK_SAMPLE_COUNT_1_BIT};
 capability_result=host_create_result=VK_SUCCESS;
 features=VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
 compatible=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
 limits=(VkImageFormatProperties){{4096,4096,256},12,256,VK_SAMPLE_COUNT_1_BIT,UINT64_MAX};
 capability_calls=plain_creates=host_creates=legacy_queries=queries2=device_queries=binds=destroys=0;
 expected_query_chain=NULL; expected_plane=0; fail_plain=false;
 physical.host_external_memory_host=true; physical.dma_buf_shim_active=false;
}
static void create_image(void) {
 struct vn_command_vkCreateImage args={.device=(VkDevice)&dev,.pCreateInfo=&expected};
 vkr_dispatch_vkCreateImage(&dispatch,&args);
 assert(args.ret==VK_SUCCESS && plain_creates==1 && legacy_queries==0);
 assert(args.pCreateInfo==&expected); // Plain creation must not inject external metadata.
}
static VkMemoryRequirements2 query_device(void) {
 VkMemoryDedicatedRequirements d={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
 VkMemoryRequirements2 out={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&d};
 VkDeviceImageMemoryRequirements info={.sType=VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
  .pCreateInfo=&expected,.planeAspect=expected_plane};
 struct vn_command_vkGetDeviceImageMemoryRequirements args={.device=(VkDevice)&dev,.pInfo=&info,.pMemoryRequirements=&out};
 vkr_dispatch_vkGetDeviceImageMemoryRequirements(&dispatch,&args);
 assert(args.pInfo==&info && info.pCreateInfo==&expected);
 assert(d.prefersDedicatedAllocation && d.requiresDedicatedAllocation==(device_queries==2));
 out.pNext=NULL;
 return out;
}
static void rejected(void) {
 create_image();
 assert(created.plain_image==HANDLE(VkImage,1) && !created.host_image && host_creates==0);
 VkMemoryRequirements2 out=query_device();
 assert(device_queries==1 && out.memoryRequirements.memoryTypeBits==9);
 VkMemoryRequirements legacy_out;
 struct vn_command_vkGetImageMemoryRequirements args={.device=(VkDevice)&dev,.image=(VkImage)&created,.pMemoryRequirements=&legacy_out};
 vkr_dispatch_vkGetImageMemoryRequirements(&dispatch,&args);
 assert(legacy_queries==1 && legacy_out.memoryTypeBits==9);
 assert(legacy_out.size==4096 && legacy_out.alignment==256);
 assert(!vkr_image_select_host_memory(&created,true));
 struct vkr_device_memory host={.host_memory=&host,.native=HANDLE(VkDeviceMemory,2)};
 struct vn_command_vkBindImageMemory bind_args={.device=(VkDevice)&dev,.image=(VkImage)&created,.memory=(VkDeviceMemory)&host};
 vkr_dispatch_vkBindImageMemory(&dispatch,&bind_args);
 assert(bind_args.ret==VK_ERROR_INVALID_EXTERNAL_HANDLE && binds==0);
 struct vkr_device_memory plain={.native=HANDLE(VkDeviceMemory,1)};
 bind_args=(struct vn_command_vkBindImageMemory){.device=(VkDevice)&dev,.image=(VkImage)&created,.memory=(VkDeviceMemory)&plain};
 vkr_dispatch_vkBindImageMemory(&dispatch,&bind_args);
 assert(bind_args.ret==VK_SUCCESS && binds==1);
}
int main(void) {
 physical.base.handle.physical_device=HANDLE(VkPhysicalDevice,20);
 physical.proc_table.GetPhysicalDeviceImageFormatProperties2=capability;
 physical.memory_properties.memoryTypeCount=4;
 physical.memory_properties.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 physical.memory_properties.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
 physical.memory_properties.memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 physical.memory_properties.memoryTypes[3].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 dev.physical_device=&physical; dev.base.handle.device=HANDLE(VkDevice,10);
 dev.proc_table=(struct vn_device_proc_table){create,destroy,legacy,requirements2,device_requirements,bind,bind2};
 reset(); capability_result=VK_ERROR_FORMAT_NOT_SUPPORTED; rejected();
 reset(); features=VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT; rejected();
 reset(); compatible=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT; rejected();
 reset(); capability_result=VK_ERROR_DEVICE_LOST; rejected();
 reset(); limits.maxExtent.width=32; rejected();
 reset(); limits.maxMipLevels=0; rejected();
 reset(); limits.maxArrayLayers=0; rejected();
 reset(); limits.sampleCounts=VK_SAMPLE_COUNT_2_BIT; rejected();
 VkBaseInStructure unknown={.sType=VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR};
 reset(); expected.pNext=&unknown; rejected(); assert(!capability_calls);
 reset(); host_create_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
 create_image(); assert(!created.host_image && host_creates==1);
 VkMemoryRequirements2 out=query_device(); (void)out; // Query may support a later successful allocation.
 assert(!vkr_image_select_host_memory(&created,true));
 reset(); fail_plain=true;
 struct vn_command_vkCreateImage failed={.device=(VkDevice)&dev,.pCreateInfo=&expected};
 vkr_dispatch_vkCreateImage(&dispatch,&failed);
 assert(failed.ret==VK_ERROR_OUT_OF_DEVICE_MEMORY && !capability_calls && !host_creates);

 reset(); create_image();
 assert(capability_calls==1 && host_creates==1 && created.host_image==HANDLE(VkImage,2));
 out=query_device(); assert(device_queries==2);
 assert(out.memoryRequirements.size==8192 && out.memoryRequirements.alignment==4096);
 assert(out.memoryRequirements.memoryTypeBits==15); // Host type 2 need not exist in the plain mask.
 VkMemoryDedicatedRequirements d={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
 VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&d};
 for (int selected=0;selected<2;selected++) {
  assert(vkr_image_select_host_memory(&created,selected));
  VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=(VkImage)&created};
  struct vn_command_vkGetImageMemoryRequirements2 args={.device=(VkDevice)&dev,.pInfo=&info,.pMemoryRequirements=&req};
  vkr_dispatch_vkGetImageMemoryRequirements2(&dispatch,&args);
  assert(req.memoryRequirements.size==8192 && req.memoryRequirements.alignment==4096 && req.memoryRequirements.memoryTypeBits==15);
  assert(d.requiresDedicatedAllocation && d.prefersDedicatedAllocation);
  VkMemoryRequirements legacy_out;
  struct vn_command_vkGetImageMemoryRequirements legacy_args={.device=(VkDevice)&dev,.image=(VkImage)&created,.pMemoryRequirements=&legacy_out};
  vkr_dispatch_vkGetImageMemoryRequirements(&dispatch,&legacy_args);
  assert(legacy_out.size==8192 && legacy_out.alignment==4096 && legacy_out.memoryTypeBits==15);
  struct vkr_device_memory memory={.host_memory=selected ? &memory : NULL,.native=HANDLE(VkDeviceMemory,selected+1)};
  struct vn_command_vkBindImageMemory bind_args={.device=(VkDevice)&dev,.image=(VkImage)&created,.memory=(VkDeviceMemory)&memory};
  vkr_dispatch_vkBindImageMemory(&dispatch,&bind_args); assert(bind_args.ret==VK_SUCCESS);
 }
 assert(queries2==4 && legacy_queries==4 && binds==2);
 assert(vkr_image_select_host_memory(&created,false)); vkr_image_release_alternate(&dev,&created); assert(destroys==1);

 // Every disjoint plane retains its own requirements and input chain. Creation
 // must never call the illegal legacy query or allocate an alternate handle.
 for (unsigned plane=0;plane<3;plane++) {
  reset(); expected.flags=VK_IMAGE_CREATE_DISJOINT_BIT;
  expected.format=VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
  create_image(); assert(!created.host_image && !host_creates && !capability_calls);
  expected_plane=VK_IMAGE_ASPECT_PLANE_0_BIT<<plane;
  VkImagePlaneMemoryRequirementsInfo plane_info={.sType=VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,.planeAspect=expected_plane};
  expected_query_chain=&plane_info;
  VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.pNext=&plane_info,.image=(VkImage)&created};
  req=(VkMemoryRequirements2){.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&d};
  struct vn_command_vkGetImageMemoryRequirements2 args={.device=(VkDevice)&dev,.pInfo=&info,.pMemoryRequirements=&req};
  vkr_dispatch_vkGetImageMemoryRequirements2(&dispatch,&args);
  VkMemoryRequirements want=requirements(false,expected_plane); want.memoryTypeBits &= 9;
  assert(req.memoryRequirements.size==want.size && req.memoryRequirements.alignment==want.alignment);
  assert(req.memoryRequirements.memoryTypeBits==want.memoryTypeBits && want.memoryTypeBits);
  assert(queries2==1 && legacy_queries==0 && !d.requiresDedicatedAllocation && d.prefersDedicatedAllocation);
  out=query_device(); assert(device_queries==1);
  assert(out.memoryRequirements.size==want.size && out.memoryRequirements.alignment==want.alignment && out.memoryRequirements.memoryTypeBits==want.memoryTypeBits);
  struct vkr_device_memory plain={.native=HANDLE(VkDeviceMemory,1)};
  VkBindImagePlaneMemoryInfo binding={.sType=VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,.planeAspect=expected_plane};
  VkBindImageMemoryInfo bind_info={.sType=VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,.pNext=&binding,.image=(VkImage)&created,.memory=(VkDeviceMemory)&plain};
  struct vn_command_vkBindImageMemory2 bind_args={.device=(VkDevice)&dev,.bindInfoCount=1,.pBindInfos=&bind_info};
  vkr_dispatch_vkBindImageMemory2(&dispatch,&bind_args); assert(bind_args.ret==VK_SUCCESS && binds==1);
 }

 // Explicit external resources and hosts without the extension keep their
 // driver requirements; no synthetic host variant or memory-mask filtering.
 VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
 reset(); expected.pNext=&external; create_image(); assert(!created.plain_image && !capability_calls && !host_creates);
 VkImageMemoryRequirementsInfo2 explicit_info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=(VkImage)&created};
 struct vn_command_vkGetImageMemoryRequirements2 explicit_args={.device=(VkDevice)&dev,.pInfo=&explicit_info,.pMemoryRequirements=&req};
 vkr_dispatch_vkGetImageMemoryRequirements2(&dispatch,&explicit_args);
 assert(queries2==1 && req.memoryRequirements.memoryTypeBits==11);
 reset(); physical.host_external_memory_host=false; create_image();
 out=query_device(); assert(!created.plain_image && !capability_calls && device_queries==1 && out.memoryRequirements.memoryTypeBits==11);
 reset(); expected.flags=VK_IMAGE_CREATE_SPARSE_BINDING_BIT; create_image();
 assert(!created.plain_image && !host_creates && !capability_calls);

 // Preserve compatible format-list and stencil-usage chains in both the
 // exact capability query and the host variant, including LINEAR tiling.
 VkImageStencilUsageCreateInfo stencil={.sType=VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,.stencilUsage=VK_IMAGE_USAGE_SAMPLED_BIT};
 VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
 VkImageFormatListCreateInfo list={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,.pNext=&stencil,.viewFormatCount=1,.pViewFormats=&format};
 reset(); expected.pNext=&list; expected.tiling=VK_IMAGE_TILING_LINEAR; create_image();
 assert(capability_calls==1 && host_creates==1 && list.pNext==&stencil);
 // The DRM shim's capability query must see the actual LINEAR host tuple.
 VkImageDrmFormatModifierListCreateInfoEXT modifier={.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
 reset(); physical.dma_buf_shim_active=true; expected.pNext=&modifier;
 // The create dispatch changes these before querying: expect the resulting tuple.
 expected.tiling=VK_IMAGE_TILING_LINEAR;
 create_image(); assert(created.dma_buf_shim_linear && capability_calls==1 && host_creates==1 && !expected.pNext);
 puts("PASS: image dispatch capability rejection, plain fallback masks, dual requirements, disjoint planes and binding, maintenance4, external passthrough");
}
'''
with tempfile.TemporaryDirectory(prefix='venus-image-capabilities-') as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
    src.write_text(harness, encoding='utf-8')
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
