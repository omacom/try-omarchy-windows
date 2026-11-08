/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The guest's own message definitions, compiled for the Windows host.
 *
 * dxgvmbus.h and the d3dkmthk.h uapi header are included from the guest
 * tree unchanged, so both sides share one definition of every message. The
 * uapi structures use 64-bit integers for pointers outside the kernel and
 * match the Windows x64 D3DKMT_* layouts; the host calls D3DKMT with them
 * directly. tests/layout_test.c checks each one against the Windows SDK.
 */
#ifndef DXGHOST_WIRE_H
#define DXGHOST_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Types the headers expect from <linux/types.h> and the kernel. */
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* The one packed uapi struct is also inside #pragma pack(1). */
#if defined(_MSC_VER)
#define __packed
#else
#define __packed __attribute__((packed))
#endif

/* d3dkmthk.h includes <linux/types.h>; compat/linux/types.h is empty. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless struct/union */
#pragma warning(disable : 4214) /* bit field types other than int */
#endif
#include "../../guest/include/uapi/misc/d3dkmthk.h"

/* From the guest's misc.h and dxgkrnl.h; dxgvmbus.h uses them. */
#define WIN_MAX_PATH 260

struct dxgk_device_types {
	u32 post_device:1;
	u32 post_device_certain:1;
	u32 software_device:1;
	u32 soft_gpu_device:1;
	u32 warp_device:1;
	u32 bdd_device:1;
	u32 support_miracast:1;
	u32 mismatched_lda:1;
	u32 indirect_display_device:1;
	u32 xbox_one_device:1;
	u32 child_id_support_dwm_clone:1;
	u32 child_id_support_dwm_clone2:1;
	u32 has_internal_panel:1;
	u32 rfx_vgpu_device:1;
	u32 virtual_render_device:1;
	u32 support_preserve_boot_display:1;
	u32 is_uefi_frame_buffer:1;
	u32 removable_device:1;
	u32 virtual_monitor_device:1;
};

enum dxgdevice_flushschedulerreason {
	DXGDEVICE_FLUSHSCHEDULER_DEVICE_TERMINATE = 4,
};

#define DXG_VIRTIO 1
struct dxgvmbuschannel;	/* named by a guest-only prototype */
#include "../../guest/dxgkrnl/dxgvmbus.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "../../protocol/virtio_dxg.h"

/* Mirrors dxgvmb_ext_header in dxgvmbus.c, which prefixes every message. */
struct dxgvmb_ext_header {
	u32 command_offset;
	u32 reserved;
	struct winluid vgpu_luid;
};

/* dxgkrnl interface version the host implements (DXGK_VMBUS_INTERFACE_VERSION). */
#define DXGHOST_INTERFACE_VERSION	40
#define DXGHOST_LAST_COMPATIBLE_VERSION	16

/* KMTQUERYADAPTERINFOTYPE values the host adapts. */
#define DXGHOST_QAI_UMDRIVERPRIVATE	0
#define DXGHOST_QAI_UMDRIVERNAME	1
#define DXGHOST_QAI_DRIVERVERSION	13
#define DXGHOST_QAI_WDDM_2_7_CAPS	70
#define DXGHOST_QAI_PHYSICALADAPTERDEVICEIDS 31
#define DXGHOST_QAI_DRIVER_DESCRIPTION	65

/* D3DKMT_QUERY_DEVICE_IDS */
struct dxghost_device_ids {
	u32 physical_adapter_index;
	u32 vendor_id;
	u32 device_id;
	u32 subvendor_id;
	u32 subsystem_id;
	u32 revision_id;
	u32 bus_type;
};

/* NTSTATUS values used by the host. */
#define DXG_STATUS_SUCCESS		((int32_t)0x00000000)
#define DXG_STATUS_PENDING		((int32_t)0x00000103)
#define DXG_STATUS_INVALID_PARAMETER	((int32_t)0xC000000D)
#define DXG_STATUS_NO_MEMORY		((int32_t)0xC0000017)
#define DXG_STATUS_INVALID_HANDLE	((int32_t)0xC0000008)
#define DXG_STATUS_NOT_SUPPORTED	((int32_t)0xC00000BB)
#define DXG_STATUS_BUFFER_TOO_SMALL	((int32_t)0xC0000023)
#define DXG_STATUS_DEVICE_REMOVED	((int32_t)0xC00002B6)
#define DXG_STATUS_ACCESS_DENIED	((int32_t)0xC0000022)

#endif /* DXGHOST_WIRE_H */
