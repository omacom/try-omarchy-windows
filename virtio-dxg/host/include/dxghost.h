/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dxghost: the Windows half of virtio-dxg.
 *
 * Executes dxgkrnl paravirtualization messages from a Linux guest with the
 * host's D3DKMT interface, so the guest's NVIDIA user-mode driver talks to
 * the real Windows NVIDIA kernel driver. Independent of any VMM: the VMM
 * moves bytes between virtqueues and dxghost_execute(), and provides
 * guest-physical mapping and event delivery through struct dxghost_vmm.
 *
 * GPU work never passes through here as data. The guest writes command
 * buffers and resources into host memory that is mapped into its physical
 * address space; only object management, submission and synchronization
 * messages cross.
 */
#ifndef DXGHOST_H
#define DXGHOST_H

#include <stdbool.h>
#include <stdint.h>
#include "../../protocol/virtio_dxg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dxghost;
struct dxghost_kmt;

struct dxghost_vmm {
	void *opaque;
	/*
	 * Make [hva, hva + size) visible to the guest at gpa, inside the IO
	 * space. Page aligned. Returns 0 on success.
	 */
	int (*map)(void *opaque, uint64_t gpa, void *hva, uint64_t size,
		   bool writable);
	void (*unmap)(void *opaque, uint64_t gpa, uint64_t size);
	/*
	 * Queue one host-to-guest packet (a dxgkvmb_command_host_to_vm) on the
	 * event queue and interrupt the guest. Called from any thread.
	 */
	void (*event)(void *opaque, const void *packet, uint32_t size);
	/* Optional diagnostics sink; NULL discards. */
	void (*log)(void *opaque, const char *message);
};

struct dxghost_config {
	/* Host adapter to expose; 0/0 selects the first NVIDIA adapter. */
	uint32_t adapter_luid_low;
	int32_t adapter_luid_high;
	/* Size of the guest-physical IO space the VMM reserved. */
	uint64_t iospace_size;
	/*
	 * Basename of the Linux UMD in the guest's driver store copy that
	 * replaces the Windows UMD name in KMTQAITYPE_UMDRIVERNAME replies.
	 * NULL derives it: nvwgf2umx.dll -> libnvwgf2umx.so.
	 */
	const char *linux_umd_name;
	/* Stable per-VM adapter identity reported to the guest. */
	uint8_t adapter_guid[16];
	/* Testing: use this table instead of gdi32.dll. */
	const struct dxghost_kmt *kmt;
};

int dxghost_create(const struct dxghost_config *config,
		   const struct dxghost_vmm *vmm, struct dxghost **out);
void dxghost_destroy(struct dxghost *host);

/* Values for the virtio device configuration space. */
void dxghost_device_config(struct dxghost *host,
			   struct virtio_dxg_config *config);

/*
 * Ordering key of a request: requests with equal keys must execute in
 * submission order; different keys may run concurrently. It is the guest
 * process the message belongs to (0 for global bookkeeping).
 */
uint32_t dxghost_lane(const void *message, uint32_t size);

/*
 * True for requests that never block the calling thread: signals,
 * submissions, GPU waits, CPU waits that complete through a guest event,
 * device state. A VMM may execute these on the thread that received them
 * (in order with the rest of their lane), saving a thread wakeup each way.
 */
bool dxghost_nonblocking(const void *message, uint32_t size);

/*
 * Executes one control request and fills the virtio reply. result may be
 * NULL when result_capacity is 0 (async requests). Thread safe.
 */
void dxghost_execute(struct dxghost *host, uint32_t channel,
		     const void *message, uint32_t size, void *result,
		     uint32_t result_capacity, struct virtio_dxg_resp *resp);

/* Guest reset or reboot: releases every object and IO-space mapping. */
void dxghost_reset(struct dxghost *host);

#ifdef __cplusplus
}
#endif

#endif /* DXGHOST_H */
