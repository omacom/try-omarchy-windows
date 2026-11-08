// SPDX-License-Identifier: MIT
/*
 * The compositor path of a d3d12 desktop, step by step: a GBM scanout
 * buffer on the KMS device, imported into EGL as a dma-buf, rendered to by
 * the GPU, and read back from the dumb buffer that KMS would scan out.
 *
 *   cc -O2 -o gbmtest gbmtest.c -lgbm -lEGL -lGLESv2
 *   GALLIUM_DRIVER=d3d12 ./gbmtest [/dev/dri/card0]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <drm/drm_mode.h>

#define W 1280
#define H 800

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror(path); return 1; }
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
	printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

	/* CROSS=1: allocate on a second open of the device, as a compositor's
	 * KMS backend does while its renderer has its own GBM device. */
	int afd = getenv("CROSS") ? open(path, O_RDWR | O_CLOEXEC) : fd;
	struct gbm_device *agbm = afd == fd ? gbm : gbm_create_device(afd);
	struct gbm_bo *bo = gbm_bo_create(agbm, W, H, GBM_FORMAT_XRGB8888,
					  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!bo) { perror("gbm_bo_create"); return 1; }
	int dmabuf = gbm_bo_get_fd(bo);
	uint32_t stride = gbm_bo_get_stride(bo), handle = gbm_bo_get_handle(bo).u32;
	printf("bo: handle %u stride %u modifier 0x%llx dmabuf fd %d\n", handle, stride,
	       (unsigned long long)gbm_bo_get_modifier(bo), dmabuf);

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "eglInitialize: 0x%x\n", eglGetError()); return 1;
	}
	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	printf("EGL %d.%d dma_buf_import %s\n", major, minor,
	       strstr(exts, "EGL_EXT_image_dma_buf_import") ? "yes" : "NO");
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint cattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
	if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "context: 0x%x\n", eglGetError()); return 1;
	}
	printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

	EGLint iattr[] = {
		EGL_WIDTH, W, EGL_HEIGHT, H,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_NONE };
	PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
	EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, iattr);
	if (img == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "eglCreateImageKHR(dma-buf): 0x%x\n", eglGetError()); return 1;
	}
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rb_storage =
		(void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	GLuint rb, fb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	rb_storage(GL_RENDERBUFFER, img);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("framebuffer status 0x%x\n", st);

	/* Read the dumb buffer that KMS would scan out. */
	struct drm_mode_map_dumb map = { .handle = handle };
	if (drmIoctl(afd, DRM_IOCTL_MODE_MAP_DUMB, &map)) { perror("MAP_DUMB"); return 1; }
	uint8_t *px = mmap(NULL, (size_t)stride * H, PROT_READ, MAP_SHARED, afd, map.offset);
	if (px == MAP_FAILED) { perror("mmap"); return 1; }

	int bad = 0;
	for (int frame = 0; frame < 5; frame++) {
		float r = 0.1f * frame, g = 0.5f, b = 0.25f;
		double t0 = now_ms();
		glViewport(0, 0, W, H);
		glClearColor(r, g, b, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();
		double t1 = now_ms();
		const uint8_t *p = px + (H / 2) * stride + (W / 2) * 4;   /* BGRX */
		int er = (int)(r * 255 + 0.5f), eg = 128, eb = 64;
		int ok = abs(p[2] - er) <= 1 && abs(p[1] - eg) <= 1 && abs(p[0] - eb) <= 1;
		bad += !ok;
		printf("frame %d: flush %.2f ms, scanout pixel %d/%d/%d (want %d/%d/%d) %s\n",
		       frame, t1 - t0, p[2], p[1], p[0], er, eg, eb, ok ? "ok" : "WRONG");
	}
	printf("%s\n", bad ? "FAIL" : "PASS: GPU frames reach the KMS buffer at glFlush");
	return bad != 0;
}
