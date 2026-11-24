// SPDX-License-Identifier: MIT
/*
 * Which GPU does Linux OpenGL get? Creates a surfaceless EGL context
 * (no window system), prints the renderer, clears a framebuffer and reads
 * it back. With /dev/dxg and Microsoft's libd3d12 present, Mesa's d3d12
 * driver renders through NVIDIA's own D3D12 driver:
 *
 *   gcc -O2 -o egltest egltest.c -ldl
 *   LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=d3d12 ./egltest
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef void *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface;
typedef int EGLint;
typedef unsigned EGLenum, EGLBoolean;

#define EGL_PLATFORM_SURFACELESS_MESA	0x31DD
#define EGL_OPENGL_API			0x30A2
#define EGL_NONE			0x3038
#define EGL_RENDERABLE_TYPE		0x3040
#define EGL_OPENGL_BIT			0x0008
#define EGL_SURFACE_TYPE		0x3033
#define EGL_CONTEXT_MAJOR_VERSION	0x3098
#define EGL_CONTEXT_MINOR_VERSION	0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK	0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x1
#define GL_VENDOR			0x1F00
#define GL_RENDERER			0x1F01
#define GL_VERSION			0x1F02
#define GL_FRAMEBUFFER			0x8D40
#define GL_RENDERBUFFER			0x8D41
#define GL_RGBA8			0x8058
#define GL_COLOR_ATTACHMENT0		0x8CE0
#define GL_COLOR_BUFFER_BIT		0x4000
#define GL_RGBA				0x1908
#define GL_UNSIGNED_BYTE		0x1401
#define GL_FRAMEBUFFER_COMPLETE		0x8CD5

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
	void *egl = dlopen("libEGL.so.1", RTLD_NOW);
	void *(*gpa)(const char *);
	EGLDisplay (*getPlatformDisplay)(EGLenum, void *, const intptr_t *);
	EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*bindAPI)(EGLenum);
	EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
				   EGLint, EGLint *);
	EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext,
				    const EGLint *);
	EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
				  EGLContext);
	const unsigned char *(*getString)(unsigned);
	void (*genFramebuffers)(int, unsigned *), (*bindFramebuffer)(unsigned, unsigned);
	void (*genRenderbuffers)(int, unsigned *), (*bindRenderbuffer)(unsigned, unsigned);
	void (*renderbufferStorage)(unsigned, unsigned, int, int);
	void (*framebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
	unsigned (*checkFramebufferStatus)(unsigned);
	void (*viewport)(int, int, int, int), (*clearColor)(float, float, float, float);
	void (*clear)(unsigned), (*finish)(void);
	void (*readPixels)(int, int, int, int, unsigned, unsigned, void *);
	const EGLint config_attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
					EGL_SURFACE_TYPE, 0, EGL_NONE };
	const EGLint context_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
		EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
	EGLDisplay dpy;
	EGLConfig config;
	EGLContext ctx;
	EGLint major, minor, n;
	unsigned fbo, rb, *pixels;
	const int w = 3840, h = 2160;
	double t, best = 1e9;
	int i, bad = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!egl) {
		printf("FAIL dlopen libEGL.so.1: %s\n", dlerror());
		return 1;
	}
	gpa = dlsym(egl, "eglGetProcAddress");
	getPlatformDisplay = (void *)gpa("eglGetPlatformDisplayEXT");
	initialize = dlsym(egl, "eglInitialize");
	bindAPI = dlsym(egl, "eglBindAPI");
	chooseConfig = dlsym(egl, "eglChooseConfig");
	createContext = dlsym(egl, "eglCreateContext");
	makeCurrent = dlsym(egl, "eglMakeCurrent");
	dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	if (!dpy || !initialize(dpy, &major, &minor) || !bindAPI(EGL_OPENGL_API) ||
	    !chooseConfig(dpy, config_attrs, &config, 1, &n) || n < 1) {
		printf("FAIL EGL setup\n");
		return 1;
	}
	ctx = createContext(dpy, config, NULL, context_attrs);
	if (!ctx || !makeCurrent(dpy, NULL, NULL, ctx)) {
		printf("FAIL OpenGL 3.3 core context\n");
		return 1;
	}
#define GL(f, name) *(void **)&f = gpa(name)
	GL(getString, "glGetString");
	GL(genFramebuffers, "glGenFramebuffers");
	GL(bindFramebuffer, "glBindFramebuffer");
	GL(genRenderbuffers, "glGenRenderbuffers");
	GL(bindRenderbuffer, "glBindRenderbuffer");
	GL(renderbufferStorage, "glRenderbufferStorage");
	GL(framebufferRenderbuffer, "glFramebufferRenderbuffer");
	GL(checkFramebufferStatus, "glCheckFramebufferStatus");
	GL(viewport, "glViewport");
	GL(clearColor, "glClearColor");
	GL(clear, "glClear");
	GL(finish, "glFinish");
	GL(readPixels, "glReadPixels");
	printf("EGL %d.%d\nGL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n",
	       major, minor, getString(GL_VENDOR), getString(GL_RENDERER),
	       getString(GL_VERSION));

	genRenderbuffers(1, &rb);
	bindRenderbuffer(GL_RENDERBUFFER, rb);
	renderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	genFramebuffers(1, &fbo);
	bindFramebuffer(GL_FRAMEBUFFER, fbo);
	framebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_RENDERBUFFER, rb);
	if (checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("FAIL framebuffer incomplete\n");
		return 1;
	}
	viewport(0, 0, w, h);
	pixels = malloc((size_t)w * h * 4);
	for (i = 0; i < 20; i++) {
		clearColor(i / 20.0f, 0.5f, 0.25f, 1.0f);
		t = now_ms();
		clear(GL_COLOR_BUFFER_BIT);
		readPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		t = now_ms() - t;
		if (t < best)
			best = t;
	}
	/* Last clear: 0.95, 0.5, 0.25 -> 242, 128, 64 (one step of rounding). */
	for (i = 0; i < w * h; i++) {
		int r = pixels[i] & 0xff, g = pixels[i] >> 8 & 0xff,
		    b = pixels[i] >> 16 & 0xff;

		bad += abs(r - 242) > 1 || abs(g - 128) > 1 || abs(b - 64) > 1;
	}
	printf("4K clear + readback: best %.2f ms; first pixel 0x%08x; %s\n",
	       best, pixels[0], bad ? "MISMATCH" : "pixels correct");
	return bad != 0;
}
