/* SPDX-License-Identifier: MIT */
/* Host-controlled GPU relay into QEMU's existing SDL/OpenGL console. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "ui/console.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include <epoxy/gl.h>
#include <glib/gstdio.h>
#include "wddm-sdl-command.h"
#include "wddm-sdl-boot.h"

#define TYPE_WDDM_SDL_RELAY "wddm-sdl-relay"
OBJECT_DECLARE_SIMPLE_TYPE(WddmSdlRelay, WDDM_SDL_RELAY)

struct WddmSdlRelay {
    DeviceState parent_obj;
    QemuConsole *con;
    QEMUGLContext import_context;
    char *session, *expected_luid, *audit_directory, *source_format, *boot_display;
    WddmSdlBoot boot;
    char vendor[128], renderer[256], version[128], luid[17];
    char *audit_error;
    QDict *audit_window_state;
    uint32_t width, height, audit_width, audit_height;
    uint64_t generation, last_frame, pending_frame, audit_frames, converted_frames;
    uint64_t source_fnv, window_fnv;
    GLuint memory, source_texture, scanout_texture, producer, consumer, channel_program;
    bool attached, ever_attached, poisoned, pending_audit, audit_called, root_verified, bgra;
    Error *migration_blocker;
};

static bool relay_gl_ok(Error **errp, const char *operation)
{
    GLenum code = glGetError();
    if (code != GL_NO_ERROR) {
        error_setg(errp, "SDL GPU relay %s failed (GL error 0x%x)", operation, code);
        return false;
    }
    return true;
}

static bool relay_adapter(WddmSdlRelay *s, Error **errp)
{
    static const char hex[] = "0123456789abcdef";
    GLubyte bytes[8];
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    g_strlcpy(s->vendor, vendor ? vendor : "", sizeof s->vendor);
    g_strlcpy(s->renderer, renderer ? renderer : "", sizeof s->renderer);
    g_strlcpy(s->version, version ? version : "", sizeof s->version);
    if (!vendor || !strstr(vendor, "NVIDIA") ||
        !epoxy_has_gl_extension("GL_EXT_memory_object") ||
        !epoxy_has_gl_extension("GL_EXT_memory_object_win32") ||
        !epoxy_has_gl_extension("GL_EXT_semaphore") ||
        !epoxy_has_gl_extension("GL_EXT_semaphore_win32")) {
        error_setg(errp, "SDL GPU relay requires NVIDIA Win32 memory/semaphore extensions; got %s / %s",
                   s->vendor, s->renderer);
        return false;
    }
    glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, bytes);
    if (!relay_gl_ok(errp, "adapter LUID")) {
        return false;
    }
    for (unsigned i = 0; i < sizeof bytes; ++i) {
        s->luid[i * 2] = hex[bytes[i] >> 4];
        s->luid[i * 2 + 1] = hex[bytes[i] & 15];
    }
    s->luid[16] = 0;
    if (strcmp(s->luid, s->expected_luid)) {
        error_setg(errp, "SDL GPU relay adapter LUID differs from the owning D3D12 producer");
        return false;
    }
    return true;
}

static void relay_delete_objects(WddmSdlRelay *s)
{
    if (s->channel_program) {
        glDeleteProgram(s->channel_program);
        s->channel_program = 0;
    }
    if (s->source_texture) {
        glDeleteTextures(1, &s->source_texture);
    }
    if (s->scanout_texture) {
        glDeleteTextures(1, &s->scanout_texture);
    }
    if (s->memory) {
        glDeleteMemoryObjectsEXT(1, &s->memory);
    }
    if (s->producer) {
        glDeleteSemaphoresEXT(1, &s->producer);
    }
    if (s->consumer) {
        glDeleteSemaphoresEXT(1, &s->consumer);
    }
    s->source_texture = s->scanout_texture = s->memory = 0;
    s->producer = s->consumer = 0;
}

static bool relay_channel_program(WddmSdlRelay *s, Error **errp)
{
    const GLchar *text =
        "#version 430 core\n"
        "layout(local_size_x=16,local_size_y=16) in;\n"
        "layout(rgba8,binding=0) readonly uniform image2D sourceImage;\n"
        "layout(rgba8,binding=1) writeonly uniform image2D targetImage;\n"
        "void main() {\n"
        "  ivec2 p=ivec2(gl_GlobalInvocationID.xy);\n"
        "  if(any(greaterThanEqual(p,imageSize(targetImage)))) return;\n"
        "  imageStore(targetImage,p,imageLoad(sourceImage,p).bgra);\n"
        "}\n";
    GLuint shader;
    GLint status = GL_FALSE;
    char log[1024] = {0};
    if (!s->bgra) {
        return true;
    }
    shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!shader) {
        error_setg(errp, "Cannot create the fixed GPU channel-conversion shader");
        return false;
    }
    glShaderSource(shader, 1, &text, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glDeleteShader(shader);
        error_setg(errp, "GPU channel-conversion compilation failed: %s", log);
        return false;
    }
    s->channel_program = glCreateProgram();
    if (!s->channel_program) {
        glDeleteShader(shader);
        error_setg(errp, "Cannot create the GPU channel-conversion program");
        return false;
    }
    glAttachShader(s->channel_program, shader);
    glLinkProgram(s->channel_program);
    glDeleteShader(shader);
    status = GL_FALSE;
    glGetProgramiv(s->channel_program, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramInfoLog(s->channel_program, sizeof log, NULL, log);
        error_setg(errp, "GPU channel-conversion link failed: %s", log);
        return false;
    }
    return relay_gl_ok(errp, "GPU channel-conversion program");
}

static bool relay_attach(WddmSdlRelay *s, Error **errp)
{
    QEMUGLParams params = { .major_ver = 4, .minor_ver = 5 };
    GLint dedicated = GL_TRUE;
    const unsigned char black[4] = {0, 0, 0, 255};
    GLsync initialized = NULL;
    g_autofree char *prefix = NULL;
    g_autofree char *name = NULL;
    g_autofree gunichar2 *memory_name = NULL;
    g_autofree gunichar2 *producer_name = NULL;
    g_autofree gunichar2 *consumer_name = NULL;

    if (!console_has_gl(s->con) || s->ever_attached || s->import_context) {
        error_setg(errp, "SDL GPU relay needs an initialized GL frontend and a fresh device/session");
        return false;
    }
    s->import_context = dpy_gl_ctx_create(s->con, &params);
    if (!s->import_context ||
        dpy_gl_ctx_make_current(s->con, s->import_context) ||
        !relay_adapter(s, errp)) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create/activate the SDL shared import context");
        }
        goto fail;
    }
    prefix = g_strdup_printf("Local\\TryOmarchy-SDL-%s-%016" PRIx64, s->session, s->generation);
    name = g_strconcat(prefix, "-texture", NULL);
    memory_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    g_clear_pointer(&name, g_free);
    name = g_strconcat(prefix, "-producer", NULL);
    producer_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    g_clear_pointer(&name, g_free);
    name = g_strconcat(prefix, "-consumer", NULL);
    consumer_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    if (!memory_name || !producer_name || !consumer_name) {
        error_setg(errp, "Cannot encode the bounded SDL GPU object names");
        goto fail;
    }
    glCreateMemoryObjectsEXT(1, &s->memory);
    glMemoryObjectParameterivEXT(s->memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32NameEXT(s->memory, 0, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, memory_name);
    glGenTextures(1, &s->source_texture);
    glBindTexture(GL_TEXTURE_2D, s->source_texture);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, s->width, s->height, s->memory, 0);
    glGenSemaphoresEXT(1, &s->producer);
    glGenSemaphoresEXT(1, &s->consumer);
    glImportSemaphoreWin32NameEXT(s->producer, GL_HANDLE_TYPE_D3D12_FENCE_EXT, producer_name);
    glImportSemaphoreWin32NameEXT(s->consumer, GL_HANDLE_TYPE_D3D12_FENCE_EXT, consumer_name);
    if (!relay_gl_ok(errp, "named GPU imports")) {
        goto fail;
    }
    glGenTextures(1, &s->scanout_texture);
    glBindTexture(GL_TEXTURE_2D, s->scanout_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, s->width, s->height);
    if (!relay_channel_program(s, errp)) {
        goto fail;
    }
    glClearTexImage(s->scanout_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    initialized = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!initialized || !relay_gl_ok(errp, "persistent GPU scanout allocation")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot fence the initial SDL GPU clear");
        }
        goto fail;
    }
    /* Host attachment can precede guest provisioning by minutes. Keep the
     * existing VGA surface until a native frame is actually presented.
     */
    if (!s->boot.console) {
        dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                              0, 0, s->width, s->height, NULL);
    }
    glWaitSync(initialized, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(initialized);
    initialized = NULL;
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    if (!relay_adapter(s, errp) || !relay_gl_ok(errp, "SDL root texture binding")) {
        if (!s->boot.console) {
            dpy_gl_scanout_disable(s->con);
        }
        goto fail;
    }
    s->attached = s->ever_attached = true;
    s->root_verified = !s->boot.console;
    return true;
fail:
    if (s->import_context) {
        if (!dpy_gl_ctx_make_current(s->con, s->import_context)) {
            if (initialized) {
                glDeleteSync(initialized);
            }
            relay_delete_objects(s);
        }
        dpy_gl_ctx_destroy(s->con, s->import_context);
        s->import_context = NULL;
    }
    return false;
}

static uint64_t relay_fnv(const unsigned char *bytes, size_t count)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count; ++i) {
        value = (value ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return value;
}

static bool relay_save(WddmSdlRelay *s, const char *kind, const void *bytes, size_t count)
{
    g_autofree char *base = g_strdup_printf("relay-%s-%016" PRIu64 ".rgba", kind, s->pending_frame);
    g_autofree char *path = g_build_filename(s->audit_directory, base, NULL);
    FILE *file = g_fopen(path, "wbx");
    bool ok;
    if (!file) {
        return false;
    }
    ok = fwrite(bytes, 1, count, file) == count;
    return fclose(file) == 0 && ok;
}

/* Called by SDL after its actual blit and before SwapWindow, in the root GL context. */
static bool relay_scanout_audit_requested(void *opaque)
{
    WddmSdlRelay *s = opaque;
    return s->pending_audit && !s->audit_called;
}

static void relay_audit_int4(QDict *state, const char *name, const GLint values[4])
{
    QDict *rect = qdict_new();
    qdict_put_int(rect, "x", values[0]);
    qdict_put_int(rect, "y", values[1]);
    qdict_put_int(rect, "width", values[2]);
    qdict_put_int(rect, "height", values[3]);
    qdict_put_obj(state, name, QOBJECT(rect));
}

static bool relay_save_window_state(WddmSdlRelay *s)
{
    g_autofree char *base = g_strdup_printf("relay-window-state-%016" PRIu64 ".json", s->pending_frame);
    g_autofree char *path = g_build_filename(s->audit_directory, base, NULL);
    GString *json = qobject_to_json(QOBJECT(s->audit_window_state));
    FILE *file = g_fopen(path, "wbx");
    bool ok = file && fwrite(json->str, 1, json->len, file) == json->len;
    if (file && fclose(file) != 0) {
        ok = false;
    }
    g_string_free(json, true);
    return ok;
}

static void relay_scanout_audit(void *opaque, uint32_t width, uint32_t height,
                                const GraphicScanoutWindowInfo *window_info)
{
    WddmSdlRelay *s = opaque;
    GLint read_fb, read_buffer, texture, pack_buffer;
    GLint packing[4];
    GLint viewport[4], scissor[4], draw_fb, draw_buffer;
    GLboolean color_mask[4], double_buffer;
    QDict *state;
    GLenum capture_error;
    const GLenum parameters[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};
    size_t source_bytes = (size_t)s->width * s->height * 4;
    size_t window_bytes = (size_t)width * height * 4;
    g_autofree unsigned char *source = NULL;
    g_autofree unsigned char *window = NULL;
    bool saved_source, saved_window, saved_state;
    bool all_zero = true;
    size_t opaque_pixels = 0, zero_alpha_pixels = 0;
    if (!s->pending_audit || s->audit_called) {
        return;
    }
    s->audit_called = true;
    qobject_unref(s->audit_window_state);
    state = s->audit_window_state = qdict_new();
    qdict_put_int(state, "schema", 1);
    qdict_put_int(state, "frame", s->pending_frame);
    qdict_put_int(state, "logicalWidth", window_info->logical_width);
    qdict_put_int(state, "logicalHeight", window_info->logical_height);
    qdict_put_int(state, "drawableWidth", window_info->drawable_width);
    qdict_put_int(state, "drawableHeight", window_info->drawable_height);
    qdict_put_int(state, "sdlWindowFlags", window_info->flags);
    qdict_put_bool(state, "shown", window_info->shown);
    qdict_put_bool(state, "hidden", window_info->hidden);
    qdict_put_bool(state, "minimized", window_info->minimized);
    qdict_put_bool(state, "maximized", window_info->maximized);
    qdict_put_bool(state, "inputFocus", window_info->input_focus);
    qdict_put_bool(state, "mouseFocus", window_info->mouse_focus);
    qdict_put_bool(state, "visibilityProvesPixelOwnership", false);
    qdict_put_bool(state, "physicalScanoutVerified", false);
    if (!width || !height || width > 3840 || height > 2160) {
        s->audit_error = g_strdup("SDL audit drawable is outside the bounded 4K extent");
        relay_save_window_state(s);
        return;
    }
    source = g_malloc0(source_bytes);
    window = g_malloc0(window_bytes);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetBooleanv(GL_DOUBLEBUFFER, &double_buffer);
    relay_audit_int4(state, "viewport", viewport);
    relay_audit_int4(state, "scissor", scissor);
    qdict_put_bool(state, "scissorEnabled", glIsEnabled(GL_SCISSOR_TEST));
    qdict_put_bool(state, "rasterizerDiscardEnabled", glIsEnabled(GL_RASTERIZER_DISCARD));
    qdict_put_bool(state, "doubleBuffered", double_buffer);
    qdict_put_int(state, "readFramebufferBeforeCapture", read_fb);
    qdict_put_int(state, "readBufferBeforeCapture", read_buffer);
    qdict_put_int(state, "drawFramebuffer", draw_fb);
    qdict_put_int(state, "drawBuffer", draw_buffer);
    qdict_put_int(state, "colorWriteMask", (color_mask[0] ? 1u : 0u) |
                                        (color_mask[1] ? 2u : 0u) |
                                        (color_mask[2] ? 4u : 0u) |
                                        (color_mask[3] ? 8u : 0u));
    for (unsigned i = 0; i < G_N_ELEMENTS(parameters); ++i) {
        glGetIntegerv(parameters[i], &packing[i]);
        glPixelStorei(parameters[i], i ? 0 : 1);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, s->scanout_texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, window);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glReadBuffer(read_buffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    for (unsigned i = 0; i < G_N_ELEMENTS(parameters); ++i) {
        glPixelStorei(parameters[i], packing[i]);
    }
    capture_error = glGetError();
    for (size_t i = 0; i < window_bytes; ++i) {
        all_zero = all_zero && window[i] == 0;
        if (i % 4 == 3) {
            opaque_pixels += window[i] == 255;
            zero_alpha_pixels += window[i] == 0;
        }
    }
    qdict_put_int(state, "captureWidth", width);
    qdict_put_int(state, "captureHeight", height);
    qdict_put_int(state, "capturePixels", window_bytes / 4);
    qdict_put_int(state, "opaqueAlphaPixels", opaque_pixels);
    qdict_put_int(state, "zeroAlphaPixels", zero_alpha_pixels);
    qdict_put_bool(state, "allBytesZero", all_zero);
    qdict_put_int(state, "glError", capture_error);
    /* Retain even rejected pixels; a failed capture must never be overwritten. */
    saved_source = relay_save(s, "source", source, source_bytes);
    saved_window = relay_save(s, "window", window, window_bytes);
    saved_state = relay_save_window_state(s);
    if (capture_error != GL_NO_ERROR || !saved_source || !saved_window || !saved_state) {
        s->audit_error = g_strdup("SDL complete GPU readback/write failed");
        return;
    }
    s->source_fnv = relay_fnv(source, source_bytes);
    s->window_fnv = relay_fnv(window, window_bytes);
    s->audit_width = width;
    s->audit_height = height;
    ++s->audit_frames;
}

static bool relay_present(WddmSdlRelay *s, uint64_t frame, bool audit, Error **errp)
{
    GLsync root_retired, copy_complete;
    GLuint64 target = frame;
    GLenum layout = GL_LAYOUT_GENERAL_EXT;
    if (!s->attached || s->poisoned || frame != s->last_frame + 1 ||
        (audit && (!s->audit_directory || !g_path_is_absolute(s->audit_directory)))) {
        error_setg(errp, "SDL GPU relay frame is stale, unowned, poisoned or lacks a bounded audit directory");
        return false;
    }
    if (s->boot.console && !s->boot.native) {
        if (!wddm_sdl_boot_enter(&s->boot)) {
            error_setg(errp, "SDL GPU relay no longer owns its boot console");
            s->poisoned = true;
            return false;
        }
        qemu_console_resize(s->con, s->width, s->height);
    }
    /*
     * Make the root current and fence ALL its previous work, including spontaneous
     * SDL redraws. Main-loop serialization prevents another redraw during this
     * update. The copy context waits on the GPU before modifying the local image.
     */
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    if (!s->root_verified) {
        if (!relay_adapter(s, errp)) {
            s->poisoned = true;
            return false;
        }
        s->root_verified = true;
    }
    root_retired = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!root_retired || !relay_gl_ok(errp, "root retirement fence")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create the SDL root retirement fence");
        }
        if (root_retired) {
            glDeleteSync(root_retired);
        }
        s->poisoned = true;
        return false;
    }
    if (dpy_gl_ctx_make_current(s->con, s->import_context)) {
        glDeleteSync(root_retired);
        error_setg(errp, "Cannot activate the SDL import context");
        s->poisoned = true;
        return false;
    }
    glWaitSync(root_retired, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(root_retired);
    glSemaphoreParameterui64vEXT(s->producer, GL_D3D12_FENCE_VALUE_EXT, &target);
    glWaitSemaphoreEXT(s->producer, 0, NULL, 1, &s->source_texture, &layout);
    if (s->bgra) {
        glUseProgram(s->channel_program);
        glBindImageTexture(0, s->source_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(1, s->scanout_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((s->width + 15) / 16, (s->height + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUseProgram(0);
    } else {
        glCopyImageSubData(s->source_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                           s->scanout_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                           s->width, s->height, 1);
    }
    glSemaphoreParameterui64vEXT(s->consumer, GL_D3D12_FENCE_VALUE_EXT, &target);
    glSignalSemaphoreEXT(s->consumer, 0, NULL, 1, &s->source_texture, &layout);
    copy_complete = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!copy_complete || !relay_gl_ok(errp, "GPU source copy and consumer signal")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create the SDL GPU copy completion fence");
        }
        if (copy_complete) {
            glDeleteSync(copy_complete);
        }
        s->poisoned = true;
        return false;
    }
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    glWaitSync(copy_complete, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(copy_complete);
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    s->pending_audit = audit;
    s->audit_called = false;
    s->pending_frame = frame;
    dpy_gl_update(s->con, 0, 0, s->width, s->height);
    s->pending_audit = false;
    if (!relay_gl_ok(errp, "SDL scanout") ||
        (audit && (!s->audit_called || s->audit_error))) {
        if (!errp || !*errp) {
            error_setg(errp, "%s", s->audit_error ? s->audit_error : "SDL did not invoke the pre-swap audit");
        }
        s->poisoned = true;
        return false;
    }
    s->last_frame = frame;
    if (s->bgra) {
        ++s->converted_frames;
    }
    return true;
}

/* Teardown-only breadcrumbs survive a process fault before the QMP reply.
 * Keep these off the presentation path and flush before each operation.
 */
static void relay_detach_stage(WddmSdlRelay *s, const char *stage)
{
    fprintf(stderr, "{\"qemuSdlDetachStage\":\"%s\",\"bootConsoleBorrowed\":%s,"
            "\"nativeConsoleActive\":%s,\"lastQueuedFrame\":%" PRIu64 "}\n",
            stage, s->boot.console ? "true" : "false",
            s->boot.native ? "true" : "false", s->last_frame);
    fflush(stderr);
}

static bool relay_detach(WddmSdlRelay *s, Error **errp)
{
    GLsync retired;
    GLenum waited;
    if (!s->attached) {
        error_setg(errp, "SDL GPU relay is not attached");
        return false;
    }
    relay_detach_stage(s, "activate-context");
    if (!s->boot.console || s->boot.native) {
        dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                              0, 0, s->width, s->height, NULL);
    } else if (dpy_gl_ctx_make_current(s->con, s->import_context)) {
        error_setg(errp, "Cannot activate the unused SDL import context for teardown");
        s->poisoned = true;
        return false;
    }
    relay_detach_stage(s, "create-fence");
    retired = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!retired) {
        error_setg(errp, "Cannot create the SDL teardown retirement fence");
        return false;
    }
    relay_detach_stage(s, "wait-fence");
    waited = glClientWaitSync(retired, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_C(5000000000));
    relay_detach_stage(s, "delete-fence");
    glDeleteSync(retired);
    if ((waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED) ||
        !relay_gl_ok(errp, "bounded teardown retirement")) {
        if (!errp || !*errp) {
            error_setg(errp, "SDL GPU relay teardown did not retire within five seconds");
        }
        s->poisoned = true;
        return false;
    }
    if (!s->boot.console || s->boot.native) {
        relay_detach_stage(s, "disable-scanout");
        dpy_gl_scanout_disable(s->con);
    }
    relay_detach_stage(s, "delete-objects");
    relay_delete_objects(s);
    if (!relay_gl_ok(errp, "shared GPU reference release")) {
        s->poisoned = true;
        return false;
    }
    relay_detach_stage(s, "delete-import-context");
    dpy_gl_ctx_destroy(s->con, s->import_context);
    s->import_context = NULL;
    s->attached = false;
    relay_detach_stage(s, "restore-boot-console");
    if (s->boot.console && !wddm_sdl_boot_leave(&s->boot)) {
        error_setg(errp, "SDL GPU relay cannot return its retired boot console");
        s->poisoned = true;
        return false;
    }
    relay_detach_stage(s, "complete");
    return true;
}

static void relay_command(Object *object, const char *text, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    WddmSdlCommand command;
    if (!DEVICE(s)->realized || !wddm_sdl_parse(text, &command) ||
        command.generation != s->generation) {
        error_setg(errp, "Invalid, stale or unrealized SDL GPU relay command");
        return;
    }
    switch (command.action) {
    case WDDM_SDL_ATTACH: relay_attach(s, errp); break;
    case WDDM_SDL_PRESENT: relay_present(s, command.frame, false, errp); break;
    case WDDM_SDL_AUDIT: relay_present(s, command.frame, true, errp); break;
    case WDDM_SDL_DETACH: relay_detach(s, errp); break;
    }
}

static char *relay_state(Object *object, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    QDict *value = qdict_new();
    GString *json;
    char source[17], window[17];
    snprintf(source, sizeof source, "%016" PRIx64, s->source_fnv);
    snprintf(window, sizeof window, "%016" PRIx64, s->window_fnv);
    qdict_put_bool(value, "attached", s->attached);
    qdict_put_bool(value, "poisoned", s->poisoned);
    qdict_put_bool(value, "everAttached", s->ever_attached);
    qdict_put_bool(value, "rootAdapterVerified", s->root_verified);
    qdict_put_bool(value, "bootConsoleBorrowed", s->boot.console != NULL);
    qdict_put_bool(value, "nativeConsoleActive", s->boot.console ? s->boot.native : s->attached);
    qdict_put_int(value, "consoleIndex", s->con ? qemu_console_get_index(s->con) : -1);
    qdict_put_bool(value, "importContextLive", s->import_context != NULL);
    qdict_put_bool(value, "localScanoutLive", s->scanout_texture != 0);
    qdict_put_bool(value, "channelProgramLive", s->channel_program != 0);
    qdict_put_str(value, "sourceFormat", s->bgra ? "bgra8" : "rgba8");
    qdict_put_int(value, "convertedFrames", s->converted_frames);
    qdict_put_int(value, "liveImportedObjects", !!s->memory + !!s->source_texture + !!s->producer + !!s->consumer);
    qdict_put_int(value, "generation", s->generation);
    qdict_put_int(value, "lastQueuedFrame", s->last_frame);
    qdict_put_int(value, "auditFrames", s->audit_frames);
    qdict_put_int(value, "sourceWidth", s->width);
    qdict_put_int(value, "sourceHeight", s->height);
    qdict_put_int(value, "windowWidth", s->audit_width);
    qdict_put_int(value, "windowHeight", s->audit_height);
    qdict_put_str(value, "vendor", s->vendor);
    qdict_put_str(value, "renderer", s->renderer);
    qdict_put_str(value, "version", s->version);
    qdict_put_str(value, "adapterLuid", s->luid);
    qdict_put_str(value, "sourceFnv1a", source);
    qdict_put_str(value, "windowFnv1a", window);
    if (s->audit_window_state) {
        qdict_put_obj(value, "windowAudit", QOBJECT(qobject_ref(s->audit_window_state)));
    }
    qdict_put_bool(value, "independentFences", true);
    qdict_put_bool(value, "cpuTextureUpload", false);
    qdict_put_bool(value, "physicalInputVerified", false);
    qdict_put_bool(value, "physicalScanoutVerified", false);
    qdict_put_bool(value, "guestGpuScanoutVerified", false);
    qdict_put_bool(value, "nearNativePerformanceVerified", false);
    json = qobject_to_json(QOBJECT(value));
    qobject_unref(value);
    return g_string_free(json, false);
}

static int relay_flags(void *opaque) { return GRAPHIC_FLAGS_GL; }
static void relay_boot_update(void *opaque)
{
    wddm_sdl_boot_update(&WDDM_SDL_RELAY(opaque)->boot);
}
static void relay_boot_invalidate(void *opaque)
{
    wddm_sdl_boot_invalidate(&WDDM_SDL_RELAY(opaque)->boot);
}
static void relay_boot_text(void *opaque, console_ch_t *text)
{
    wddm_sdl_boot_text(&WDDM_SDL_RELAY(opaque)->boot, text);
}
static const GraphicHwOps relay_hw_ops = {
    .gl_scanout_audit_requested = relay_scanout_audit_requested,
    .get_flags = relay_flags,
    .gfx_update = relay_boot_update,
    .invalidate = relay_boot_invalidate,
    .text_update = relay_boot_text,
    .gl_scanout_audit = relay_scanout_audit,
};

static void relay_realize(DeviceState *device, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(device);
    if (!wddm_sdl_hex(s->session, 16) || !wddm_sdl_hex(s->expected_luid, 8) ||
        !s->generation || s->generation > WDDM_SDL_MAX_COUNTER ||
        !s->width || !s->height || s->width > 3840 || s->height > 2160) {
        error_setg(errp, "SDL GPU relay needs a fresh 128-bit session, adapter LUID, generation and bounded 4K extent");
        return;
    }
    if (s->source_format && strcmp(s->source_format, "rgba8") && strcmp(s->source_format, "bgra8")) {
        error_setg(errp, "SDL GPU relay needs rgba8 or bgra8 source-format");
        return;
    }
    s->bgra = s->source_format && !strcmp(s->source_format, "bgra8");
    error_setg(&s->migration_blocker, "SDL GPU relay's native resource ownership is not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return;
    }
    if (s->boot_display) {
        Object *owner;
        s->con = qemu_console_lookup_by_device_name(s->boot_display, 0, errp);
        if (!s->con) {
            return;
        }
        owner = object_property_get_link(OBJECT(s->con), "device", &error_abort);
        if (!owner || !object_dynamic_cast(owner, "VGA") ||
            qemu_console_get_index(s->con) != 0) {
            error_setg(errp, "SDL GPU relay boot-display requires the primary standard VGA console");
            s->con = NULL;
            return;
        }
        for (unsigned i = 1; qemu_console_lookup_by_index(i); ++i) {
            if (qemu_console_is_graphic(qemu_console_lookup_by_index(i))) {
                error_setg(errp, "SDL GPU relay boot-display requires one graphic console");
                s->con = NULL;
                return;
            }
        }
        if (!wddm_sdl_boot_bind(&s->boot, s->con, &relay_hw_ops, s)) {
            error_setg(errp, "SDL GPU relay boot-display is asynchronous, GL-backed or already owned");
            s->con = NULL;
            return;
        }
    } else {
        s->con = graphic_console_init(device, 0, &relay_hw_ops, s);
        qemu_console_resize(s->con, s->width, s->height);
    }
}

static void relay_unrealize(DeviceState *device)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(device);
    Error *error = NULL;
    if (s->attached && !relay_detach(s, &error)) {
        error_report_err(error);
    }
    if (s->boot.console) {
        /* Even a failed GL retirement must not leave callbacks into a freed
         * relay. Imported objects are retained on failure until process exit.
         */
        if (s->boot.native) {
            dpy_gl_scanout_disable(s->con);
        }
        if (!wddm_sdl_boot_restore(&s->boot)) {
            error_report("SDL GPU relay lost boot-console ownership during unrealize");
        } else {
            graphic_hw_invalidate(s->con);
        }
    } else if (s->con) {
        graphic_console_close(s->con);
    }
    s->con = NULL;
}

static void relay_finalize(Object *object)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
    g_free(s->audit_error);
    qobject_unref(s->audit_window_state);
}
static const Property relay_properties[] = {
    DEFINE_PROP_STRING("session", WddmSdlRelay, session),
    DEFINE_PROP_STRING("adapter-luid", WddmSdlRelay, expected_luid),
    DEFINE_PROP_STRING("audit-directory", WddmSdlRelay, audit_directory),
    DEFINE_PROP_STRING("source-format", WddmSdlRelay, source_format),
    DEFINE_PROP_STRING("boot-display", WddmSdlRelay, boot_display),
    DEFINE_PROP_UINT32("width", WddmSdlRelay, width, 3840),
    DEFINE_PROP_UINT32("height", WddmSdlRelay, height, 2160),
    DEFINE_PROP_UINT64("generation", WddmSdlRelay, generation, 1),
};
static void relay_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(klass);
    device->realize = relay_realize;
    device->unrealize = relay_unrealize;
    device->hotpluggable = false;
    device->user_creatable = true;
    device->desc = "Host-owned named D3D12 GPU texture relay to the existing SDL console";
    device_class_set_props(device, relay_properties);
    object_class_property_add_str(klass, "relay-command", NULL, relay_command);
    object_class_property_add_str(klass, "relay-state", relay_state, NULL);
}
static const TypeInfo relay_info = {
    .name = TYPE_WDDM_SDL_RELAY, .parent = TYPE_DEVICE,
    .instance_size = sizeof(WddmSdlRelay), .instance_finalize = relay_finalize,
    .class_init = relay_class_init,
};
static void relay_register(void) { type_register_static(&relay_info); }
type_init(relay_register);
