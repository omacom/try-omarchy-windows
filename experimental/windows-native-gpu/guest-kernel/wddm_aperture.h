/* SPDX-License-Identifier: MIT */
/* Private QEMU bridge ABI. Optional native buffers use genuine DMA-BUF files;
 * this still does not implement a DRM device or /dev/dxg driver. */
#ifndef WDDM_APERTURE_H
#define WDDM_APERTURE_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define WDDM_APERTURE_VERSION 1
#define WDDM_APERTURE_FENCE 1
#define WDDM_APERTURE_ALLOCATION 2
#define WDDM_APERTURE_WRITE 1
#define WDDM_APERTURE_DEFAULT_LEASES 256
#define WDDM_APERTURE_MAX_LEASES 512
/* Only the explicit 256-fence hub needs room for both CPU and fence views. */
static inline __u32 wddm_aperture_lease_limit(__u32 fence_slots)
{
    return fence_slots == 256 ? WDDM_APERTURE_MAX_LEASES : WDDM_APERTURE_DEFAULT_LEASES;
}
#define WDDM_APERTURE_CPU_LIMIT (96ULL * 1024 * 1024)
#define WDDM_APERTURE_DESKTOP_CPU_LIMIT (192ULL * 1024 * 1024)
#define WDDM_APERTURE_MULTIAPP_CPU_LIMIT (384ULL * 1024 * 1024)
/* Additive discovery leaves the original INFO structure/ABI unchanged. */
#define WDDM_APERTURE_DESKTOP_CPU_ENABLED 1U
#define WDDM_APERTURE_SPARSE_ENABLED 2U
#define WDDM_APERTURE_MULTIAPP_CPU_ENABLED 4U
struct wddm_aperture_caps {
	__u32 version, flags;
	__u64 cpu_byte_limit, reserved[2];
};
static inline int wddm_aperture_valid_caps(struct wddm_aperture_caps caps)
{
	return caps.version == 1 && !caps.reserved[0] && !caps.reserved[1] &&
		((!caps.flags && caps.cpu_byte_limit == WDDM_APERTURE_CPU_LIMIT) ||
		 ((caps.flags == WDDM_APERTURE_DESKTOP_CPU_ENABLED || caps.flags == (WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED)) &&
		  caps.cpu_byte_limit == WDDM_APERTURE_DESKTOP_CPU_LIMIT) ||
		 (caps.flags == (WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED|WDDM_APERTURE_MULTIAPP_CPU_ENABLED) &&
		  caps.cpu_byte_limit == WDDM_APERTURE_MULTIAPP_CPU_LIMIT));
}
#define WDDM_APERTURE_CAPS _IOWR('W', 0x50, struct wddm_aperture_caps)
/* Separate immutable opt-in: original INFO/CAPS and byte budgets stay fixed. */
#define WDDM_APERTURE_COORDINATED_LEASES 768U
/* Metadata for the explicit 512-fence + 1024-allocation layout. Byte limits
 * remain independent; legacy coordinated guests keep their 768-record bound. */
#define WDDM_APERTURE_DESKTOP_LEASES (512U + 1024U)
struct wddm_aperture_coordinator_caps { __u32 version, enabled, lease_limit, reserved; };
static inline int wddm_aperture_valid_coordinator_caps(struct wddm_aperture_coordinator_caps caps)
{
	return caps.version == 1 && !caps.reserved && caps.enabled <= 1 &&
		(caps.enabled ? (caps.lease_limit == WDDM_APERTURE_COORDINATED_LEASES || caps.lease_limit == WDDM_APERTURE_DESKTOP_LEASES) :
		 caps.lease_limit == WDDM_APERTURE_DEFAULT_LEASES || caps.lease_limit == WDDM_APERTURE_MAX_LEASES);
}
#define WDDM_APERTURE_COORDINATOR_CAPS _IOWR('W', 0x51, struct wddm_aperture_coordinator_caps)
/* Additive discovery. A 512-page physical fence BAR needs a separate immutable
 * module opt-in; the original INFO/CAPS/coordinator payload sizes stay fixed. */
struct wddm_aperture_desktop_fences {
    __u32 version, enabled, slots, stride;
    __u64 bytes, reserved;
};
static inline int wddm_aperture_valid_desktop_fences(struct wddm_aperture_desktop_fences c)
{
    return c.version==1 && c.enabled<=1 && !c.reserved && c.stride==4096 &&
        c.bytes==(__u64)c.slots*4096 &&
        (c.enabled ? c.slots==512 : (c.slots==64 || c.slots==128 || c.slots==256));
}
#define WDDM_APERTURE_DESKTOP_FENCES _IOWR('W', 0x52, struct wddm_aperture_desktop_fences)
/* Separate sparse CPU address capacity; mapped-byte and legacy discovery
 * contracts stay unchanged. A larger BAR does not authorize extra data. */
struct wddm_aperture_desktop_slots {
    __u32 version, enabled, slots, stride;
    __u64 bytes, reserved;
};
static inline int wddm_aperture_valid_desktop_slots(struct wddm_aperture_desktop_slots c)
{
    return c.version==1 && c.enabled<=1 && !c.reserved && c.stride==1048576 &&
        c.bytes==(__u64)c.slots*1048576 &&
        (c.enabled ? c.slots==1024 : (c.slots>=16 && c.slots<=512 && !(c.slots&(c.slots-1))));
}
#define WDDM_APERTURE_DESKTOP_SLOTS _IOWR('W', 0x53, struct wddm_aperture_desktop_slots)
#define WDDM_OBJECT_SYNC 1
#define WDDM_OBJECT_RESOURCE 2
#define WDDM_OBJECT_MAX 256

struct wddm_aperture_info {
	__u32 version, fence_slots, allocation_slots, active_leases;
	__u64 fence_bytes, allocation_bytes, cpu_slot_bytes, active_cpu_bytes;
	__u64 fence_mmio_reads, allocation_mmio_reads, allocation_mmio_writes;
};
struct wddm_aperture_create {
	__u32 version, kind;
	__u64 offset, bytes, host_generation, cookie;
	__s32 fd;
	__u32 flags;
	__u64 reserved[2];
};
struct wddm_aperture_revoke {
	__u32 version, reserved;
	__u64 cookie;
};

#define WDDM_APERTURE_INFO _IOWR('W', 0x40, struct wddm_aperture_info)
#define WDDM_APERTURE_CREATE _IOWR('W', 0x41, struct wddm_aperture_create)
#define WDDM_APERTURE_REVOKE _IOW('W', 0x42, struct wddm_aperture_revoke)

/* Opaque bridge-session tickets, never native HANDLEs, CPU mappings,
 * dma-bufs or sync_files. Only the trusted controller can resolve/reap. */
struct wddm_object_create {
	__u32 version, kind;
	__u64 cookie;
	__u32 ticket;
	__s32 fd;
	__u64 reserved[2];
};
struct wddm_object_resolve {
	__u32 version, kind;
	__s32 fd;
	__u32 ticket;
	__u64 cookie, reserved;
};
struct wddm_object_reap {
	__u32 version, kind, ticket, reserved;
	__u64 cookie;
};
struct wddm_object_stats {
	__u32 version, held, active, reserved;
	__u64 created, closed, revoked;
};
#define WDDM_OBJECT_CREATE _IOWR('W', 0x43, struct wddm_object_create)
#define WDDM_OBJECT_RESOLVE _IOWR('W', 0x44, struct wddm_object_resolve)
#define WDDM_OBJECT_REVOKE _IOW('W', 0x45, struct wddm_aperture_revoke)
#define WDDM_OBJECT_REAP _IOWR('W', 0x46, struct wddm_object_reap)
#define WDDM_OBJECT_STATS _IOWR('W', 0x47, struct wddm_object_stats)

/* Explicit experimental sync_file export from an authorized read-only native
 * monitored-fence lease. No DMA-BUF or EGL-native-fence import is implied. */
struct wddm_sync_file_create {
	__u32 version, flags;
	__u64 cookie, offset, value;
	__s32 fd;
	__u32 reserved;
};
struct wddm_sync_file_stats {
	__u32 version, held, pending, reserved;
	__u64 created, signaled, errors, released;
};
#define WDDM_SYNC_FILE_CREATE _IOWR('W', 0x48, struct wddm_sync_file_create)
#define WDDM_SYNC_FILE_STATS _IOWR('W', 0x49, struct wddm_sync_file_stats)
/* Additive opt-in lineage ABI. The retained descriptor must be a genuine
 * WDDM_OBJECT_SYNC capability; no user-provided ticket/Windows HANDLE. */
struct wddm_sync_file_native_create {
	struct wddm_sync_file_create sync;
	__s32 native_fd;
	__u32 reserved;
};
struct wddm_sync_file_native_resolve {
	__u32 version, flags;
	__s32 fd, native_fd;
	__u64 value, reserved;
};
#define WDDM_SYNC_FILE_NATIVE_CREATE _IOWR('W', 0x4a, struct wddm_sync_file_native_create)
#define WDDM_SYNC_FILE_NATIVE_RESOLVE _IOWR('W', 0x4b, struct wddm_sync_file_native_resolve)

/* Opt-in opaque native-resource DMA-BUF. bytes is a bounded logical extent,
 * not a physical layout/pitch, CPU mapping or foreign DMA attachment promise.
 * Only the same privileged controller may wrap/query its actual resource FD. */
#define WDDM_NATIVE_BUFFER_MAX 64
#define WDDM_NATIVE_BUFFER_MAX_BYTES (256ULL * 1024 * 1024)
#define WDDM_NATIVE_BUFFER_TOTAL_BYTES (1024ULL * 1024 * 1024)
struct wddm_native_buffer_create {
	__u32 version, flags;
	__s32 native_fd, fd;
	__u64 bytes, reserved;
};
struct wddm_native_buffer_info {
	__u32 version, flags;
	__s32 fd;
	__u32 reserved;
	__u64 bytes;
};
struct wddm_native_buffer_stats {
	__u32 version, held;
	__u64 bytes, created, released;
};
#define WDDM_NATIVE_BUFFER_CREATE _IOWR('W', 0x4c, struct wddm_native_buffer_create)
#define WDDM_NATIVE_BUFFER_INFO _IOWR('W', 0x4d, struct wddm_native_buffer_info)
#define WDDM_NATIVE_BUFFER_STATS _IOWR('W', 0x4e, struct wddm_native_buffer_stats)
/* Read-only feature discovery; never infer support from a zero object count. */
#define WDDM_NATIVE_BUFFER_ENABLED 1U
struct wddm_native_buffer_caps {
	__u32 version, flags, reserved[2];
};
#define WDDM_NATIVE_BUFFER_CAPS _IOWR('W', 0x4f, struct wddm_native_buffer_caps)

/* Asynchronous CPU notification through restricted fence-lease capabilities.
 * event_fd is captured by the kernel; FD reuse cannot redirect a notification.
 * No Windows handle, controller FD or writable GPU fence is exposed. */
#define WDDM_FENCE_EVENTFD_MAX_POINTS 64U
#define WDDM_FENCE_EVENTFD_MAX_WAITS 64U
#define WDDM_FENCE_EVENTFD_WAIT_ANY 1U
struct wddm_fence_eventfd_point {
	__s32 lease_fd;
	__u32 reserved;
	__u64 offset, value;
};
struct wddm_fence_eventfd_wait {
	__u32 version, flags, count;
	__s32 event_fd;
	__u64 points, reserved[2];
};
struct wddm_fence_eventfd_stats {
	__u32 version, held, pending, reserved;
	__u64 created, notified, errors, released;
};
#define WDDM_FENCE_EVENTFD_WAIT _IOW('W', 0x50, struct wddm_fence_eventfd_wait)
#define WDDM_FENCE_EVENTFD_STATS _IOWR('W', 0x51, struct wddm_fence_eventfd_stats)
#endif
