// SPDX-License-Identifier: MIT
/*
 * Zero-copy client buffers, both ends in one process: an application
 * allocates a window buffer through GBM on the render node, renders into it
 * with the GPU and exports it, as Mesa's Wayland EGL does; a compositor (GBM
 * and EGL on the KMS node, as Hyprland) imports the descriptor as a dma-buf
 * EGL image and reads the pixels back.
 *
 *   cc -O2 -o clientbuftest clientbuftest.c $(pkg-config --cflags libdrm) \
 *       -lgbm -lEGL -lGLESv2
 *   MESA_LOADER_DRIVER_OVERRIDE=kms_swrast ./clientbuftest
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 640
#define H 480

struct side {
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
};

static int side_init(struct side *s, const char *node)
{
	static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLConfig cfg = NULL;
	EGLint n = 0;
	int fd = open(node, O_RDWR | O_CLOEXEC);

	if (fd < 0) { perror(node); return -1; }
	s->gbm = gbm_create_device(fd);
	if (!s->gbm) { fprintf(stderr, "%s: no GBM device\n", node); return -1; }
	s->dpy = get_display(EGL_PLATFORM_GBM_KHR, s->gbm, NULL);
	if (!eglInitialize(s->dpy, NULL, NULL)) {
		fprintf(stderr, "%s: eglInitialize 0x%x\n", node, eglGetError());
		return -1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	eglChooseConfig(s->dpy, cfg_attr, &cfg, 1, &n);
	s->ctx = eglCreateContext(s->dpy, n ? cfg : NULL, EGL_NO_CONTEXT, ctx_attr);
	if (!s->ctx) {
		fprintf(stderr, "%s: eglCreateContext 0x%x\n", node, eglGetError());
		return -1;
	}
	return 0;
}

static GLuint fbo_of_image(EGLImageKHR img)
{
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target =
		(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	GLuint tex, fbo;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	target(GL_TEXTURE_2D, img);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	return fbo;
}

int main(void)
{
	PFNEGLCREATEIMAGEKHRPROC create_image =
		(void *)eglGetProcAddress("eglCreateImageKHR");
	struct side app, comp;
	/* FORMAT=AB24 (as Chromium), XR24, XB24; AR24 by default. */
	const char *f = getenv("FORMAT") ?: "AR24";
	uint32_t fourcc = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
	int frame, bad = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (side_init(&app, getenv("APP_NODE") ?: "/dev/dri/renderD128") ||
	    side_init(&comp, getenv("COMP_NODE") ?: "/dev/dri/card0"))
		return 1;

	/* The application: a window buffer, as wl_egl allocates one. */
	struct gbm_bo *bo = gbm_bo_create(app.gbm, W, H, fourcc,
					  GBM_BO_USE_RENDERING);
	if (!bo) { perror("gbm_bo_create on the render node"); return 1; }
	int fd = gbm_bo_get_fd(bo);
	uint32_t stride = gbm_bo_get_stride(bo);
	uint64_t modifier = gbm_bo_get_modifier(bo);
	printf("application buffer: fd %d, stride %u, modifier 0x%llx\n", fd,
	       stride, (unsigned long long)modifier);
	if (fd < 0) { fprintf(stderr, "no descriptor\n"); return 1; }

	EGLint attr[] = {
		EGL_WIDTH, W, EGL_HEIGHT, H,
		EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32),
		EGL_NONE };

	/* VIA_DMABUF=1: the application renders through its own import of the
	 * buffer on a second display, as Chromium's GPU process does (GBM to
	 * allocate, its GL's EGL to render). */
	if (getenv("VIA_DMABUF")) {
		struct side render;

		if (side_init(&render, getenv("APP_NODE") ?: "/dev/dri/renderD128"))
			return 1;
		app.dpy = render.dpy;
		app.ctx = render.ctx;
	}
	eglMakeCurrent(app.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, app.ctx);
	printf("application renders on: %s%s\n", glGetString(GL_RENDERER),
	       getenv("VIA_DMABUF") ? " (through a dma-buf import)" : "");
	EGLImageKHR app_img = getenv("VIA_DMABUF") ?
		create_image(app.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr) :
		create_image(app.dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
	if (!app_img) { fprintf(stderr, "app image 0x%x\n", eglGetError()); return 1; }
	GLuint app_fbo = fbo_of_image(app_img);

	/* The compositor: import what linux-dmabuf would carry. */
	eglMakeCurrent(comp.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, comp.ctx);
	EGLImageKHR comp_img = create_image(comp.dpy, EGL_NO_CONTEXT,
					    EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	if (!comp_img) {
		fprintf(stderr, "FAIL: compositor import 0x%x\n", eglGetError());
		return 1;
	}
	GLuint comp_fbo = fbo_of_image(comp_img);
	printf("compositor imports on: %s\n", glGetString(GL_RENDERER));

	for (frame = 1; frame <= 4; frame++) {
		unsigned char px[4] = {0};
		int want = frame * 60;

		eglMakeCurrent(app.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, app.ctx);
		glBindFramebuffer(GL_FRAMEBUFFER, app_fbo);
		glViewport(0, 0, W, H);
		glClearColor(want / 255.0f, 0.5f, 0.25f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();	/* as eglSwapBuffers before wl_surface.commit */

		eglMakeCurrent(comp.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, comp.ctx);
		glBindFramebuffer(GL_FRAMEBUFFER, comp_fbo);
		glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		int ok = abs(px[0] - want) <= 1 && abs(px[1] - 128) <= 1;
		printf("frame %d: compositor sees %u/%u/%u (want %d/128/64) %s\n",
		       frame, px[0], px[1], px[2], want, ok ? "ok" : "WRONG");
		bad += !ok;
	}
	printf("%s\n", bad ? "FAIL" :
	       "PASS: the compositor reads the application's GPU frames without a copy");
	return bad != 0;
}
