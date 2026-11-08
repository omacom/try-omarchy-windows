/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * virtio-dxg wire contract, shared by the Linux guest driver and the Windows
 * host device.
 *
 * virtio-dxg carries Microsoft's dxgkrnl paravirtualization messages (the
 * dxgvmbus.h command set that WSL2 sends over VMBus) on a virtio transport,
 * so an unmodified dxgkrnl message layer and NVIDIA's WSL user-mode driver
 * can run inside a QEMU/WHPX guest. Only framing lives here. The payloads
 * are the dxgkvmb_command_* structures from dxgvmbus.h, byte for byte.
 *
 * Layout rules: little-endian, naturally aligned, no implicit padding.
 */
#ifndef VIRTIO_DXG_H
#define VIRTIO_DXG_H

#ifdef __KERNEL__
#include <linux/types.h>
#define VIRTIO_DXG_LE32 __le32
#define VIRTIO_DXG_LE64 __le64
#else
#include <stdint.h>
#define VIRTIO_DXG_LE32 uint32_t
#define VIRTIO_DXG_LE64 uint64_t
#endif

/*
 * Private device ID. Not assigned by OASIS; chosen outside the range Linux
 * 7.2 defines in include/uapi/linux/virtio_ids.h. The modern virtio-pci
 * device ID is 0x1040 + VIRTIO_ID_DXG.
 */
#define VIRTIO_ID_DXG			62
#define VIRTIO_DXG_PCI_DEVICE_ID	(0x1040 + VIRTIO_ID_DXG)

/* Feature bits (device-specific range 0..23). */
/* The host maps guest page lists (dxgk_vmbus_hostcaps.map_guest_memory). */
#define VIRTIO_DXG_F_GUEST_PAGES	0
/* The host may answer requests out of submission order. */
#define VIRTIO_DXG_F_OUT_OF_ORDER	1

/* Virtqueues. */
#define VIRTIO_DXG_VQ_CTRL		0	/* guest -> host requests */
#define VIRTIO_DXG_VQ_EVENT		1	/* host -> guest packets */
#define VIRTIO_DXG_VQ_COUNT		2

/*
 * Shared memory region that backs dxgkrnl's "IO space". The host maps
 * CPU-visible GPU allocations, monitored-fence pages and hardware-queue
 * progress fences into it, and returns guest physical addresses inside it.
 */
#define VIRTIO_DXG_SHM_IOSPACE		0

#define VIRTIO_DXG_CFG_COMPUTE_ONLY	(1u << 0)

/* Device configuration space. Only guest_caps is written by the driver. */
struct virtio_dxg_config {
	VIRTIO_DXG_LE32 vmbus_version;	 /* dxgkrnl interface version offered */
	VIRTIO_DXG_LE32 host_caps;	 /* struct dxgk_vmbus_hostcaps */
	VIRTIO_DXG_LE32 guest_caps;	 /* struct dxgk_vmbus_guestcaps */
	VIRTIO_DXG_LE32 flags;		 /* VIRTIO_DXG_CFG_* */
	VIRTIO_DXG_LE32 vgpu_luid_low;	 /* host adapter LUID */
	VIRTIO_DXG_LE32 vgpu_luid_high;
	unsigned char	adapter_guid[16]; /* stable guest-visible adapter id */
	VIRTIO_DXG_LE32 max_message;	 /* largest request or reply, bytes */
	VIRTIO_DXG_LE32 event_size;	 /* largest host -> guest packet, bytes */
};

/* Which dxgkrnl channel a request belongs to. */
#define VIRTIO_DXG_CHANNEL_GLOBAL	0
#define VIRTIO_DXG_CHANNEL_ADAPTER	1

#define VIRTIO_DXG_REQ_SYNC		0	/* reply expected */
#define VIRTIO_DXG_REQ_ASYNC		1	/* no reply, ordered */

/*
 * Control request: one device-readable segment holding this header followed
 * by the dxgkrnl message, then (sync only) one device-writable segment
 * holding struct virtio_dxg_resp followed by result_size bytes.
 */
struct virtio_dxg_req {
	VIRTIO_DXG_LE32 kind;		/* VIRTIO_DXG_REQ_* */
	VIRTIO_DXG_LE32 channel;	/* VIRTIO_DXG_CHANNEL_* */
	VIRTIO_DXG_LE32 size;		/* dxgkrnl message bytes that follow */
	VIRTIO_DXG_LE32 result_size;	/* bytes the guest can receive */
};

/* Transport status. A dxgkrnl NTSTATUS, when present, is inside the result. */
#define VIRTIO_DXG_OK			0
#define VIRTIO_DXG_ERR_INVALID		1	/* malformed framing */
#define VIRTIO_DXG_ERR_UNSUPPORTED	2	/* unknown command */
#define VIRTIO_DXG_ERR_DEVICE		3	/* host backend failed */
#define VIRTIO_DXG_ERR_OVERFLOW		4	/* reply larger than result_size */
/*
 * The command failed with resp.ntstatus. Used for replies that have no
 * NTSTATUS slot of their own (Escape returns only private data); the
 * result bytes are still delivered.
 */
#define VIRTIO_DXG_ERR_NTSTATUS		5

struct virtio_dxg_resp {
	VIRTIO_DXG_LE32 status;		/* VIRTIO_DXG_* */
	VIRTIO_DXG_LE32 size;		/* result bytes written */
	VIRTIO_DXG_LE32 ntstatus;	/* with VIRTIO_DXG_ERR_NTSTATUS */
	VIRTIO_DXG_LE32 reserved;
};

/*
 * Event queue: the guest posts device-writable buffers of config.event_size
 * bytes. Each used buffer holds this header followed by one
 * dxgkvmb_command_host_to_vm packet (SIGNALGUESTEVENT, SETGUESTDATA, ...).
 */
struct virtio_dxg_event {
	VIRTIO_DXG_LE32 size;		/* packet bytes that follow */
	VIRTIO_DXG_LE32 reserved;
};

#define VIRTIO_DXG_MAX_MESSAGE		(128 * 1024)
#define VIRTIO_DXG_EVENT_SIZE		256

#endif /* VIRTIO_DXG_H */
