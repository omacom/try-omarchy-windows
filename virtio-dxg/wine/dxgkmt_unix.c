// SPDX-License-Identifier: MIT
/*
 * Linux half of the D3DKMT bridge: a Wine Unix library.
 *
 * Wine routes system calls made by Windows code to its NT emulation, so the
 * Windows half (kmt_bridge.c, dxgvk.c) reaches Linux through these entry
 * points, called with __wine_unix_call: /dev/dxg, window buffers for
 * swapchain images, and their presentation in an X11 window.
 *
 *   gcc -shared -fPIC -O2 -o dxgkmt.so dxgkmt_unix.c \
 *       -lgbm -lxcb -lxcb-dri3 -lxcb-present -lpthread
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <gbm.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
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

/* --- Window buffers and X11 -------------------------------------------- */

struct virtio_winbuf {
	uint32_t op;		/* 3: lookup */
	int32_t dmabuf_fd;
	int32_t resource_fd;
	uint32_t reserved;
};
#define LX_DXVIRTIOWINBUF _IOWR(0x47, 0x81, struct virtio_winbuf)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gbm_device *gbm;
static xcb_connection_t *x;
static uint32_t present_serial;

static int open_gbm(void)
{
	int i;

	if (gbm)
		return 0;
	for (i = 128; i < 136 && !gbm; i++) {
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		gbm = gbm_create_device(fd);
		if (!gbm)
			close(fd);
	}
	return gbm ? 0 : -ENODEV;
}

static int open_x(void)
{
	if (!x) {
		x = xcb_connect(NULL, NULL);
		if (xcb_connection_has_error(x)) {
			xcb_disconnect(x);
			x = NULL;
		}
	}
	return x ? 0 : -ENOTCONN;
}

static uint32_t window_depth(uint32_t window)
{
	xcb_get_geometry_reply_t *g;
	uint32_t depth = 24;

	if (!window || open_x())
		return depth;
	g = xcb_get_geometry_reply(x, xcb_get_geometry(x, window), NULL);
	if (g) {
		depth = g->depth;
		free(g);
	}
	return depth;
}

static NTSTATUS unix_winbuf_create(void *args)
{
	struct dxgkmt_winbuf *p = args;
	struct virtio_winbuf look = { 3, -1, -1, 0 };
	struct gbm_bo *bo;
	int ret;

	pthread_mutex_lock(&lock);
	p->dmabuf_fd = p->resource_fd = -1;
	ret = open_gbm();
	if (ret)
		goto out;
	p->depth = window_depth(p->x_window);
	bo = gbm_bo_create(gbm, p->width, p->height,
			   p->depth == 32 ? GBM_FORMAT_ARGB8888 :
					    GBM_FORMAT_XRGB8888,
			   GBM_BO_USE_RENDERING);
	if (!bo) {
		ret = -ENOMEM;
		goto out;
	}
	p->stride = gbm_bo_get_stride(bo);
	p->dmabuf_fd = gbm_bo_get_fd(bo);
	if (p->dmabuf_fd < 0) {
		gbm_bo_destroy(bo);
		ret = -EIO;
		goto out;
	}
	/* The allocation keeps the GPU texture alive until the image goes:
	 * nothing else in this process holds it. */
	p->bo = (uint64_t)(uintptr_t)bo;
	look.dmabuf_fd = p->dmabuf_fd;
	if (ioctl(dxg_fd, LX_DXVIRTIOWINBUF, &look) || look.resource_fd < 0) {
		ret = -errno;	/* no zero-copy window buffers here */
		close(p->dmabuf_fd);
		p->dmabuf_fd = -1;
		gbm_bo_destroy(bo);
		p->bo = 0;
		goto out;
	}
	p->resource_fd = look.resource_fd;
out:
	pthread_mutex_unlock(&lock);
	p->ret = ret;
	return 0;
}

/* Present completion per window, for FIFO pacing. */
struct xwin {
	uint32_t window;
	xcb_special_event_t *events;
	uint32_t last, completed;	/* serials */
};
static struct xwin wins[32];

static struct xwin *xwin_of(uint32_t window)
{
	struct xwin *free_slot = NULL;
	uint32_t eid;
	int i;

	for (i = 0; i < 32; i++) {
		if (wins[i].window == window)
			return &wins[i];
		if (!wins[i].window && !free_slot)
			free_slot = &wins[i];
	}
	if (!free_slot)
		free_slot = &wins[window % 32];	/* reuse: pacing is best effort */
	if (free_slot->events)
		xcb_unregister_for_special_event(x, free_slot->events);
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->window = window;
	eid = xcb_generate_id(x);
	xcb_present_select_input(x, eid, window,
				 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
	free_slot->events = xcb_register_for_special_xge(x, &xcb_present_id, eid,
							 NULL);
	return free_slot;
}

static void drain(struct xwin *w)
{
	xcb_generic_event_t *e;

	while (w->events && (e = xcb_poll_for_special_event(x, w->events))) {
		xcb_present_generic_event_t *g = (void *)e;

		if (g->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
			xcb_present_complete_notify_event_t *c = (void *)e;

			if (c->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP)
				w->completed = c->serial;
		}
		free(e);
	}
}

/* Until the window's previous frame is on screen (at most 100 ms). */
static void wait_previous(struct xwin *w)
{
	int i;

	for (i = 0; i < 400 && w->completed != w->last; i++) {
		drain(w);
		if (w->completed == w->last)
			break;
		usleep(250);
	}
}

static NTSTATUS unix_x_present(void *args)
{
	struct dxgkmt_present *p = args;
	struct xwin *w;
	int ret;

	pthread_mutex_lock(&lock);
	ret = open_x();
	if (ret)
		goto out;
	if (p->locate) {
		xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(x)).data;
		xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(
			x, xcb_translate_coordinates(x, s->root, p->x_window,
						     p->root_x, p->root_y), NULL);

		p->x = t ? t->dst_x : 0;
		p->y = t ? t->dst_y : 0;
		/* The window's own origin when Wine's screen is not the root's. */
		if (p->x < 0 || p->y < 0)
			p->x = p->y = 0;
		free(t);
	}
	w = xwin_of(p->x_window);
	if (p->wait)
		wait_previous(w);
	else
		drain(w);
	if (!p->pixmap) {
		int fd = fcntl(p->dmabuf_fd, F_DUPFD_CLOEXEC, 3);

		if (fd < 0) {
			ret = -errno;
			goto out;
		}
		p->pixmap = xcb_generate_id(x);
		/* The X server takes the descriptor. */
		xcb_dri3_pixmap_from_buffer(x, p->pixmap, p->x_window,
					    p->stride * p->height, p->width,
					    p->height, p->stride, p->depth, 32,
					    fd);
	}
	w->last = ++present_serial;
	xcb_present_pixmap(x, p->x_window, p->pixmap, w->last,
			   0, 0, p->x, p->y, 0, 0, 0,
			   p->wait ? XCB_PRESENT_OPTION_NONE : XCB_PRESENT_OPTION_ASYNC,
			   0, 0, 0, 0, NULL);
	xcb_flush(x);
	if (xcb_connection_has_error(x))
		ret = -EPIPE;
out:
	pthread_mutex_unlock(&lock);
	p->ret = ret;
	return 0;
}

static NTSTATUS unix_x_free(void *args)
{
	struct dxgkmt_present *p = args;

	pthread_mutex_lock(&lock);
	if (x && p->pixmap) {
		xcb_free_pixmap(x, p->pixmap);
		xcb_flush(x);
	}
	pthread_mutex_unlock(&lock);
	p->ret = 0;
	return 0;
}

static NTSTATUS unix_winbuf_destroy(void *args)
{
	struct dxgkmt_winbuf *p = args;

	pthread_mutex_lock(&lock);
	if (p->bo)
		gbm_bo_destroy((struct gbm_bo *)(uintptr_t)p->bo);
	pthread_mutex_unlock(&lock);
	p->bo = 0;
	p->ret = 0;
	return 0;
}

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[] = {
	[DXGKMT_UNIX_OPEN] = unix_open,
	[DXGKMT_UNIX_IOCTL] = unix_ioctl,
	[DXGKMT_UNIX_EVENTFD] = unix_eventfd,
	[DXGKMT_UNIX_READ_EVENT] = unix_read_event,
	[DXGKMT_UNIX_CLOSE] = unix_close,
	[DXGKMT_UNIX_WINBUF_CREATE] = unix_winbuf_create,
	[DXGKMT_UNIX_X_PRESENT] = unix_x_present,
	[DXGKMT_UNIX_X_FREE] = unix_x_free,
	[DXGKMT_UNIX_WINBUF_DESTROY] = unix_winbuf_destroy,
};
