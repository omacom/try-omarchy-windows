/* passbench [SHADOWS=50] [FRAMES=300] [REUSE=0]: GTK 4's GPU renderer's
 * pattern for CSS drop shadows, per frame: for each shadow, a new 96x96
 * texture and framebuffer, clear, draw the icon into it; a second texture
 * and framebuffer, clear, draw the first blurred; then draw both into the
 * 1024x1024 target; textures and framebuffers deleted at the end of the
 * frame; glFinish. Prints CPU time per frame and per shadow.
 * REUSE=1 keeps textures/framebuffers across frames (isolates creation).
 * Surfaceless EGL. Build: gcc -O2 passbench.c -lEGL -lGL */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *vs =
	"#version 330\n"
	"uniform vec4 r; out vec2 uv;\n"
	"const vec2 p[4] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));\n"
	"void main() { uv = p[gl_VertexID]; gl_Position = vec4(r.xy + uv * r.zw, 0, 1); }\n";
static const char *fs_icon =
	"#version 330\n"
	"uniform sampler2D t; in vec2 uv; out vec4 c;\n"
	"void main() { c = texture(t, uv); }\n";
static const char *fs_blur =
	"#version 330\n"
	"uniform sampler2D t; in vec2 uv; out vec4 c;\n"
	"void main() { vec2 d = vec2(1.0 / 96.0); vec4 s = vec4(0);\n"
	"  for (int i = -1; i <= 1; i++) for (int j = -1; j <= 1; j++) s += texture(t, uv + vec2(i, j) * d);\n"
	"  c = vec4(0, 0, 0, 0.3 * s.a / 9.0); }\n";

static GLuint program(const char *f)
{
	GLuint p = glCreateProgram(), s;
	s = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(s, 1, &vs, NULL);
	glCompileShader(s);
	glAttachShader(p, s);
	s = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(s, 1, &f, NULL);
	glCompileShader(s);
	glAttachShader(p, s);
	glLinkProgram(p);
	return p;
}

static int64_t now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : x > y;
}

static void tex(GLuint t, int w, int h, const void *data)
{
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

int main(int argc, char **argv)
{
	int shadows = argc > 1 ? atoi(argv[1]) : 50;
	int frames = argc > 2 ? atoi(argv[2]) : 300;
	int reuse = argc > 3 ? atoi(argv[3]) : 0;
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	eglInitialize(d, NULL, NULL);
	eglBindAPI(EGL_OPENGL_API);
	EGLint attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
			  EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attr);
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
	GLuint picon = program(fs_icon), pblur = program(fs_blur), vao, target, tfb, icon;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	uint32_t *px = malloc(64 * 64 * 4);
	for (int i = 0; i < 64 * 64; i++)
		px[i] = (i % 64 > 8 && i % 64 < 56 && i / 64 > 4 && i / 64 < 60) ? 0xffbbbbbb : 0;
	glGenTextures(1, &icon);
	tex(icon, 64, 64, px);
	glGenTextures(1, &target);
	tex(target, 1024, 1024, NULL);
	glGenFramebuffers(1, &tfb);
	glBindFramebuffer(GL_FRAMEBUFFER, tfb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	GLint ricon = glGetUniformLocation(picon, "r"), rblur = glGetUniformLocation(pblur, "r");
	GLuint *t = calloc(shadows * 2, sizeof(GLuint)), *f = calloc(shadows * 2, sizeof(GLuint));
	int64_t *ft = calloc(frames, sizeof(int64_t));
	int64_t *gt = calloc(frames, sizeof(int64_t));
	GLuint qs[2];
	glGenQueries(2, qs);
	for (int fr = 0; fr < frames + 10; fr++) {
		int64_t t0 = now();
		glBeginQuery(GL_TIME_ELAPSED, qs[0]);
		for (int s = 0; s < shadows; s++) {
			GLuint *a = &t[s * 2], *fa = &f[s * 2];
			if (!reuse || fr == 0) {
				glGenTextures(2, a);
				tex(a[0], 96, 96, NULL);
				tex(a[1], 96, 96, NULL);
				glGenFramebuffers(2, fa);
				for (int k = 0; k < 2; k++) {
					glBindFramebuffer(GL_FRAMEBUFFER, fa[k]);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a[k], 0);
					glCheckFramebufferStatus(GL_FRAMEBUFFER);
				}
			}
			/* Offscreen 1: the icon. */
			glBindFramebuffer(GL_FRAMEBUFFER, fa[0]);
			glViewport(0, 0, 96, 96);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glUseProgram(picon);
			glBindTexture(GL_TEXTURE_2D, icon);
			glUniform4f(ricon, -0.8f, -0.8f, 1.6f, 1.6f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* Offscreen 2: blurred. */
			glBindFramebuffer(GL_FRAMEBUFFER, fa[1]);
			glClear(GL_COLOR_BUFFER_BIT);
			glUseProgram(pblur);
			glBindTexture(GL_TEXTURE_2D, a[0]);
			glUniform4f(rblur, -1, -1, 2, 2);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* Into the target: shadow, then the icon. */
			glBindFramebuffer(GL_FRAMEBUFFER, tfb);
			glViewport(0, 0, 1024, 1024);
			float x = -1 + (s % 9) * 0.22f, y = -1 + (s / 9) * 0.3f + (fr % 30) * 0.01f;
			glBindTexture(GL_TEXTURE_2D, a[1]);
			glUniform4f(rblur, x, y - 0.005f, 0.18f, 0.18f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glUseProgram(picon);
			glBindTexture(GL_TEXTURE_2D, a[0]);
			glUniform4f(ricon, x, y, 0.18f, 0.18f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		if (!reuse) {
			glDeleteFramebuffers(shadows * 2, f);
			glDeleteTextures(shadows * 2, t);
		}
		glEndQuery(GL_TIME_ELAPSED);
		glFinish();
		if (fr >= 10) {
			GLuint64 el;
			ft[fr - 10] = now() - t0;
			glGetQueryObjectui64v(qs[0], GL_QUERY_RESULT, &el);
			gt[fr - 10] = (int64_t)el;
		}
	}
	qsort(ft, frames, sizeof(*ft), cmp);
	qsort(gt, frames, sizeof(*gt), cmp);
	printf("GPU time per frame p50 %.3f ms (%.1f us per shadow)\n", gt[frames / 2] / 1e6,
	       gt[frames / 2] / 1e3 / (shadows ? shadows : 1));
	printf("shadows %d reuse %d: frame p50 %.2f ms p90 %.2f ms; per shadow %.1f us (p50)\n",
	       shadows, reuse, ft[frames / 2] / 1e6, ft[frames * 9 / 10] / 1e6,
	       ft[frames / 2] / 1e3 / shadows);
	return 0;
}
