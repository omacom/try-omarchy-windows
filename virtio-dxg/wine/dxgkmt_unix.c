// SPDX-License-Identifier: MIT
/*
 * Linux half of the D3DKMT bridge: a Wine Unix library.
 *
 * Wine routes system calls made by Windows code to its NT emulation, so the
 * Windows half (kmt_bridge.c) reaches /dev/dxg through these entry points,
 * called with __wine_unix_call. Each one is a single system call.
 *
 *   gcc -shared -fPIC -O2 -o dxgkmt.so dxgkmt_unix.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "dxgkmt_unix.h"

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);

static int dxg_fd = -1;

static NTSTATUS unix_open(void *args)
{
	struct dxgkmt_result *p = args;

	if (dxg_fd < 0)
		dxg_fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
	p->ret = dxg_fd < 0 ? -errno : 0;
	return 0;
}

static NTSTATUS unix_ioctl(void *args)
{
	struct dxgkmt_ioctl *p = args;
	int r;

	do {
		r = ioctl(dxg_fd, (unsigned long)p->request,
			  (void *)(uintptr_t)p->arg);
	} while (r < 0 && errno == EINTR);
	p->ret = r < 0 ? -errno : r;
	return 0;
}

static NTSTATUS unix_eventfd(void *args)
{
	struct dxgkmt_result *p = args;
	int fd = eventfd(0, EFD_CLOEXEC);

	p->ret = fd < 0 ? -errno : fd;
	return 0;
}

static NTSTATUS unix_read_event(void *args)
{
	struct dxgkmt_fd *p = args;
	uint64_t value;
	ssize_t r;

	do {
		r = read((int)p->fd, &value, sizeof(value));
	} while (r < 0 && errno == EINTR);
	p->ret = r < 0 ? -errno : 0;
	return 0;
}

static NTSTATUS unix_close(void *args)
{
	struct dxgkmt_fd *p = args;

	p->ret = close((int)p->fd) < 0 ? -errno : 0;
	return 0;
}

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[] = {
	[DXGKMT_UNIX_OPEN] = unix_open,
	[DXGKMT_UNIX_IOCTL] = unix_ioctl,
	[DXGKMT_UNIX_EVENTFD] = unix_eventfd,
	[DXGKMT_UNIX_READ_EVENT] = unix_read_event,
	[DXGKMT_UNIX_CLOSE] = unix_close,
};
