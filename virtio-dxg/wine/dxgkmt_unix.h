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

#endif
