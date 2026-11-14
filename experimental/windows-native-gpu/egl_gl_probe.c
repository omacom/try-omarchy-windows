// SPDX-License-Identifier: MIT
// Guest OpenGL integration probe. This does not create a desktop or display.
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Resolve the desktop GL dispatch from the EGL implementation that owns the
// current context. This also supports a non-GLVND private Mesa installation.
#define GL_FUNCTIONS(X) \
    X(GETSTRING, GetString) X(GETINTEGERV, GetIntegerv) X(CREATESHADER, CreateShader) \
    X(SHADERSOURCE, ShaderSource) X(COMPILESHADER, CompileShader) X(GETSHADERIV, GetShaderiv) \
    X(GETSHADERINFOLOG, GetShaderInfoLog) X(DELETESHADER, DeleteShader) X(CREATEPROGRAM, CreateProgram) \
    X(ATTACHSHADER, AttachShader) X(LINKPROGRAM, LinkProgram) X(GETPROGRAMIV, GetProgramiv) \
    X(USEPROGRAM, UseProgram) X(GETUNIFORMLOCATION, GetUniformLocation) X(GENVERTEXARRAYS, GenVertexArrays) \
    X(BINDVERTEXARRAY, BindVertexArray) X(VIEWPORT, Viewport) X(DISABLE, Disable) \
    X(PIXELSTOREI, PixelStorei) X(CLEARCOLOR, ClearColor) X(CLEAR, Clear) X(UNIFORM1I, Uniform1i) \
    X(DRAWARRAYS, DrawArrays) X(FINISH, Finish) X(READPIXELS, ReadPixels) X(GETERROR, GetError) \
    X(DELETEVERTEXARRAYS, DeleteVertexArrays) X(DELETEPROGRAM, DeleteProgram) X(GETSTRINGI, GetStringi)
#define DECLARE_GL(upper, name) static PFNGL##upper##PROC gl##name;
GL_FUNCTIONS(DECLARE_GL)
#undef DECLARE_GL

enum { Width = 130, Height = 73, Frames = 2 };
#ifdef WDDM_BRIDGE_GBM_TEST
extern EGLDisplay wddm_gbm_native_display(void);
extern int wddm_gbm_native_bind(EGLDisplay);
extern int wddm_gbm_native_sample(void);
extern int wddm_gbm_native_cleanup(void);
extern int wddm_gbm_native_close(void);
#endif
#ifdef WDDM_BRIDGE_SYNC_FD_TEST
extern int wddm_sync_fd_test(void);
#endif
#ifdef WDDM_BRIDGE_NATIVE_SYNC_FILE_TEST
extern int wddm_native_sync_file_test(EGLDisplay, EGLContext);
#endif
#ifdef WDDM_BRIDGE_MESA_FENCE_FD_TEST
extern int wddm_mesa_fence_fd_test(EGLDisplay, EGLContext);
#endif
#ifdef WDDM_BRIDGE_MESA_RESOURCE_FD_TEST
extern int wddm_mesa_resource_fd_test(EGLDisplay, EGLContext);
#endif
#ifdef WDDM_BRIDGE_NATIVE_IMAGE_TEST
extern int wddm_native_image_test(EGLDisplay, EGLContext);
extern int wddm_native_image_cleanup(void);
#endif
static int fail(const char* stage) {
    printf("OPENGL_FAILURE stage=%s eglError=%04x\n", stage, eglGetError());
    return 1;
}
static GLuint shader(GLenum type, const char* source) {
    GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, NULL);
    glCompileShader(result);
    GLint success = 0;
    glGetShaderiv(result, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[2048] = {0};
        glGetShaderInfoLog(result, sizeof log, NULL, log);
        printf("OPENGL_SHADER_FAILURE type=%u log=%s\n", type, log);
        glDeleteShader(result);
        return 0;
    }
    return result;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef WDDM_BRIDGE_GBM_TEST
    if (setenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE", "true", 1) ||
        setenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS", "true", 1)) return fail("GbmOptIn");
#endif
#if (defined(WDDM_BRIDGE_EGL_NATIVE_SYNC_TEST) && !defined(WDDM_BRIDGE_EGL_NATIVE_SYNC_DISABLED)) || defined(WDDM_BRIDGE_RESOURCE_DMABUF_TEST) || defined(WDDM_BRIDGE_NATIVE_IMAGE_TEST)
    if (setenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE", "true", 1)) return fail("NativeSyncOptIn");
#endif
#ifdef WDDM_BRIDGE_NATIVE_IMAGE_TEST
    if(setenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS","true",1)) return fail("NativeBufferOptIn");
#endif
#ifdef WDDM_BRIDGE_MESA_FENCE_FD_TEST
    // Explicit private lab opt-in, before Mesa initializes screen caps.
    if (setenv("D3D12_EXPERIMENTAL_FENCE_FD", "true", 1)) return fail("FenceFdOptIn");
#endif
    puts("OPENGL_TEST_BEGIN api=OpenGL requested=3.3 width=130 height=73 frames=2 display=false");
#if defined(WDDM_BRIDGE_MESA_FENCE_FD_TEST) || defined(WDDM_BRIDGE_MESA_RESOURCE_FD_TEST) || defined(WDDM_BRIDGE_NATIVE_SYNC_FILE_TEST) || defined(WDDM_BRIDGE_NATIVE_IMAGE_TEST) || defined(WDDM_BRIDGE_GBM_TEST)
    const char* begin_name = "wddm_bridge_begin_opengl_fences";
#else
    const char* begin_name = "wddm_bridge_begin_opengl";
#endif
    int (*begin_profile)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, begin_name);
    if (!begin_profile || begin_profile()) return fail("BridgeProfile");
#ifdef WDDM_BRIDGE_GBM_TEST
    EGLDisplay display = wddm_gbm_native_display();
#else
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
#endif
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) return fail("Initialize");
    printf("OPENGL_EGL version=%d.%d vendor=%s\n", major, minor, eglQueryString(display, EGL_VENDOR));
    printf("OPENGL_EGL_EXTENSIONS %s\n", eglQueryString(display, EGL_EXTENSIONS));
    if (!eglBindAPI(EGL_OPENGL_API)) return fail("BindAPI");
#ifdef WDDM_BRIDGE_GBM_TEST
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
#else
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
#endif
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return fail("ChooseConfig");
#ifdef WDDM_BRIDGE_GBM_TEST
    EGLSurface surface = EGL_NO_SURFACE;
#else
    const EGLint surface_attributes[] = {EGL_WIDTH, Width, EGL_HEIGHT, Height, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (surface == EGL_NO_SURFACE) return fail("Pbuffer");
#endif
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) return fail("Context");
#define LOAD_GL(upper, name) gl##name = (PFNGL##upper##PROC)eglGetProcAddress("gl" #name); if (!gl##name) return fail("gl" #name);
    GL_FUNCTIONS(LOAD_GL)
#undef LOAD_GL
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    printf("OPENGL_IDENTITY renderer=%s\n", renderer ? renderer : "(null)");
    printf("OPENGL_VERSION %s\n", version ? version : "(null)");
    printf("OPENGL_VENDOR %s\n", vendor ? vendor : "(null)");
    if (!renderer || !strstr(renderer, "D3D12") || !strstr(renderer, "NVIDIA") ||
        strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) return fail("HardwareRenderer");
    GLint gl_major = 0, gl_minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &gl_minor);
    if (gl_major < 3 || (gl_major == 3 && gl_minor < 3)) return fail("OpenGL33");
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint index = 0; index < extensions; ++index) {
        const char* extension = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)index);
        if (extension && (strstr(extension, "memory_object") || strstr(extension, "semaphore") ||
            strstr(extension, "EGL_image"))) printf("OPENGL_INTEROP_EXTENSION %s\n", extension);
    }
    const char* vertex_source = "#version 330 core\n"
        "uniform int rotation; out vec3 color;\n"
        "void main(){vec2 positions[3]=vec2[3](vec2(-.75,-.75),vec2(0,.75),vec2(.75,-.75));"
        "vec3 colors[3]=vec3[3](vec3(1,0,0),vec3(0,1,0),vec3(0,0,1));"
        "gl_Position=vec4(positions[gl_VertexID],.5,1);color=colors[(gl_VertexID+rotation)%3];}\n";
    const char* fragment_source = "#version 330 core\n"
        "in vec3 color; layout(location=0) out vec4 outputColor; void main(){outputColor=vec4(color,1);}\n";
    GLuint vertex = shader(GL_VERTEX_SHADER, vertex_source), fragment = shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) return fail("Shaders");
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return fail("Link");
    glUseProgram(program);
    GLint rotation = glGetUniformLocation(program, "rotation");
    if (rotation < 0) return fail("Uniform");
    GLuint array = 0;
    glGenVertexArrays(1, &array); glBindVertexArray(array);
    glViewport(0, 0, Width, Height);
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#ifdef WDDM_BRIDGE_GBM_TEST
    if (wddm_gbm_native_bind(display)) return fail("GbmBuffers");
    glViewport(0, 0, Width, Height);
#endif
    unsigned char pixels[Width * Height * 4];
    for (unsigned frame = 1; frame <= Frames; ++frame) {
        glClearColor(0, 0, frame == 1 ? 1 : 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(rotation, (GLint)frame - 1); glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        glReadPixels(0, 0, Width, Height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        GLenum error = glGetError();
        printf("OPENGL_FRAME frame=%u vertices=3 error=%04x\n", frame, error);
        if (error != GL_NO_ERROR) return fail("DrawReadback");
        for (unsigned y = 0; y < Height; ++y) {
            printf("OPENGL_PIXEL_ROW frame=%u y=%u rgba=", frame, y);
            for (unsigned x = 0; x < Width * 4; ++x) printf("%02x", pixels[(Height - 1 - y) * Width * 4 + x]);
            putchar('\n');
        }
    }
#ifdef WDDM_BRIDGE_SYNC_FD_TEST
    if (wddm_sync_fd_test()) return fail("SharedSyncFd");
#endif
#ifdef WDDM_BRIDGE_MESA_FENCE_FD_TEST
    if (wddm_mesa_fence_fd_test(display, context)) return fail("MesaFenceFd");
#endif
#ifdef WDDM_BRIDGE_NATIVE_IMAGE_TEST
    if(wddm_native_image_test(display,context)) return fail("NativeImage");
#endif
#ifdef WDDM_BRIDGE_GBM_TEST
    if (wddm_gbm_native_sample()) return fail("GbmSample");
    if (wddm_gbm_native_cleanup()) return fail("GbmCleanup");
#endif
    glDeleteVertexArrays(1, &array); glUseProgram(0); glDeleteProgram(program);
    glDeleteShader(vertex); glDeleteShader(fragment);
#ifdef WDDM_BRIDGE_MESA_RESOURCE_FD_TEST
    if (wddm_mesa_resource_fd_test(display, context)) return fail("SharedResourceFd");
#endif
#ifdef WDDM_BRIDGE_NATIVE_SYNC_FILE_TEST
    if (wddm_native_sync_file_test(display, context)) return fail("NativeSyncFile");
#endif
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ||
        !eglDestroyContext(display, context) || (surface != EGL_NO_SURFACE && !eglDestroySurface(display, surface)) || !eglTerminate(display))
        return fail("Teardown");
#ifdef WDDM_BRIDGE_GBM_TEST
    if (wddm_gbm_native_close()) return fail("GbmClose");
#endif
#ifdef WDDM_BRIDGE_NATIVE_IMAGE_TEST
    if(wddm_native_image_cleanup()) return fail("NativeImageCleanup");
#endif
#ifdef WDDM_BRIDGE_EXIT_LIFETIME_TEST
    // Leave the DSO loaded. Its ELF finalizer runs after the bridge's atexit
    // hook; retaining an existing CPU fence view must remain safe until exit.
    void* lifetime = dlopen("/runtime-exit-probe.so", RTLD_NOW | RTLD_LOCAL);
    int (*arm_lifetime)(void) = lifetime ? (int (*)(void))dlsym(lifetime, "wddm_exit_lifetime_arm") : NULL;
    if (!arm_lifetime || arm_lifetime()) return fail("ExitLifetime");
#endif
    puts("OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false");
    return 0;
}
