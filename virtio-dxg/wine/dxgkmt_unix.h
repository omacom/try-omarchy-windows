/* SPDX-License-Identifier: MIT */
/* Calls from the Windows half of the D3DKMT bridge into dxgkmt.so. */
#ifndef DXGKMT_UNIX_H
#define DXGKMT_UNIX_H

#include <stdint.h>

enum dxgkmt_unix_call {
	DXGKMT_UNIX_OPEN,	/* struct dxgkmt_result */
	DXGKMT_UNIX_IOCTL,	/* struct dxgkmt_ioctl */
	DXGKMT_UNIX_EVENTFD,	/* struct dxgkmt_result: the new fd */
	DXGKMT_UNIX_READ_EVENT,	/* struct dxgkmt_fd: blocks until signaled */
	DXGKMT_UNIX_CLOSE,	/* struct dxgkmt_fd */
	DXGKMT_UNIX_WINBUF_CREATE,	/* struct dxgkmt_winbuf */
	DXGKMT_UNIX_X_PRESENT,	/* struct dxgkmt_present */
	DXGKMT_UNIX_X_FREE,	/* struct dxgkmt_present: pixmap only */
	DXGKMT_UNIX_WINBUF_DESTROY,	/* struct dxgkmt_winbuf: bo only */
};

/* ret is >= 0 on success, -errno on failure. */
struct dxgkmt_result {
	int64_t ret;
};

struct dxgkmt_ioctl {
	uint64_t request;
	uint64_t arg;
	int64_t ret;
};

struct dxgkmt_fd {
	int64_t fd;
	int64_t ret;
};

/*
 * A window buffer for a swapchain image: a shared GPU texture (Mesa's
 * D3D12 driver allocates it through GBM) behind a virtio-gpu dma-buf.
 * resource_fd is the texture's dxgkrnl NT handle, which the Vulkan driver
 * imports; dmabuf_fd is what X11 and the compositor get.
 */
struct dxgkmt_winbuf {
	uint32_t width, height;
	uint32_t x_window;	/* in: its depth selects the format */
	uint32_t stride, depth;	/* out */
	int32_t dmabuf_fd, resource_fd;	/* out */
	uint64_t bo;		/* out: the allocation, until WINBUF_DESTROY */
	int64_t ret;
};

/* Shows a window buffer in an X11 window (DRI3 pixmap + Present). */
struct dxgkmt_present {
	uint32_t x_window;
	uint32_t pixmap;	/* in/out: 0 creates it from dmabuf_fd */
	int32_t dmabuf_fd;
	uint32_t width, height, stride, depth;
	int32_t x, y;		/* where in the window (out when locate) */
	int32_t root_x, root_y;	/* locate: that point on the X screen */
	uint32_t locate;	/* turns root_x/root_y into x/y */
	uint32_t wait;		/* FIFO: one frame in flight per window */
	int64_t ret;
};

#endif
