/* Additional CPU callbacks for the actual st_upload_constants functions.
 * No shader, graphics library, driver, window or device is involved. */
typedef unsigned mesa_shader_stage;
typedef float GLfloat;
typedef union { float f; uint32_t u; } gl_constant_value;
enum {
    MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
    MESA_SHADER_GEOMETRY, MESA_SHADER_FRAGMENT, MESA_SHADER_COMPUTE,
    MESA_SHADER_TASK, MESA_SHADER_MESH,
    MAX_INLINABLE_UNIFORMS=4, MAX_NUM_FRAGMENT_CONSTANTS_ATI=2,
    GL_OUT_OF_MEMORY=0x0505
};
struct pipe_constant_buffer {
    struct pipe_resource *buffer;
    const void *user_buffer;
    unsigned buffer_offset, buffer_size;
};
struct gl_program_parameter { unsigned ValueOffset; };
struct gl_program_parameter_list {
    unsigned NumParameters, NumParameterValues, StateFlags;
    int UniformBytes;
    gl_constant_value *ParameterValues;
    struct gl_program_parameter *Parameters;
};
struct ati_fragment_shader {
    unsigned LocalConstDef;
    GLfloat Constants[MAX_NUM_FRAGMENT_CONSTANTS_ATI][4];
};
struct gl_program {
    struct gl_program_parameter_list *Parameters;
    struct ati_fragment_shader *ati_fs;
    struct { unsigned num_inlinable_uniforms, inlinable_uniform_dw_offsets[4]; } info;
};
struct gl_context {
    struct { unsigned UniformBufferOffsetAlignment; } Const;
    struct { GLfloat GlobalConstants[MAX_NUM_FRAGMENT_CONSTANTS_ATI][4]; } ATIFragmentShader;
};
struct st_context {
    struct pipe_context *pipe;
    struct gl_context *ctx;
    struct { unsigned constbuf0_enabled_shader_mask; } state;
    bool prefer_real_buffer_in_constbuf0;
    struct pipe_resource *deferred[8];
    unsigned deferred_count;
};
static struct {
    unsigned errors, invalid_copy, invalid_state, invalid_bind, unbind, bind;
    unsigned copies, uploaded_state, loaded_state, inlinable, subroutine;
    unsigned samplers, images;
    struct pipe_resource *bound[8];
    unsigned offset[8], size[8];
    const void *user[8];
    uint32_t inline_values[4];
} tracker;
static void *checked_memcpy(void *dest, const void *source, size_t bytes)
{
    if (bytes && !dest) { ++tracker.invalid_copy; return dest; }
    ++tracker.copies;
    return memcpy(dest, source, bytes);
}
static void set_constant_buffer(struct pipe_context *pipe, unsigned stage,
                                unsigned index, const struct pipe_constant_buffer *cb)
{
    (void)pipe;
    if (stage>=8 || index!=0) { ++model.model_error; return; }
    if (!cb) {
        ++tracker.unbind;
        pipe_resource_reference(&tracker.bound[stage], NULL);
        tracker.user[stage]=NULL;
        return;
    }
    ++tracker.bind;
    if ((!cb->buffer && !cb->user_buffer) || (cb->buffer &&
        (!cb->buffer->refs || cb->buffer_offset>cb->buffer->width0 ||
         cb->buffer_size>cb->buffer->width0-cb->buffer_offset))) ++tracker.invalid_bind;
    pipe_resource_reference(&tracker.bound[stage], cb->buffer);
    tracker.offset[stage]=cb->buffer_offset;
    tracker.size[stage]=cb->buffer_size;
    tracker.user[stage]=cb->user_buffer;
}
static void set_inlinable_constants(struct pipe_context *pipe, unsigned stage,
                                    unsigned count, const uint32_t *values)
{
    (void)pipe; (void)stage;
    if (count>4) { ++model.model_error; return; }
    ++tracker.inlinable;
    memcpy(tracker.inline_values, values, count*sizeof(*values));
}
static void st_add_releasebuf(struct st_context *st, struct pipe_resource *resource)
{
    if (!resource) return;
    if (st->deferred_count>=8) { ++model.model_error; return; }
    st->deferred[st->deferred_count++]=resource;
}
static void drain_releasebufs(struct st_context *st)
{
    for (unsigned i=0; i<st->deferred_count; ++i)
        pipe_resource_release(st->pipe, st->deferred[i]);
    st->deferred_count=0;
}
static inline void _mesa_error(struct gl_context *ctx, unsigned error, const char *message, ...)
{
    (void)ctx;
    if (error!=GL_OUT_OF_MEMORY || strcmp(message,"st_upload_constants")) ++model.model_error;
    ++tracker.errors;
}
static void _mesa_shader_write_subroutine_indices(struct gl_context *ctx, unsigned stage)
{ (void)ctx; (void)stage; ++tracker.subroutine; }
static void st_make_bound_samplers_resident(struct st_context *st, struct gl_program *prog)
{ (void)st; (void)prog; ++tracker.samplers; }
static void st_make_bound_images_resident(struct st_context *st, struct gl_program *prog)
{ (void)st; (void)prog; ++tracker.images; }
static void write_state(struct gl_program_parameter_list *params, uint32_t *dest)
{
    unsigned start=(unsigned)params->UniformBytes/4;
    for (unsigned i=0; i<4; ++i) dest[start+i]=0xfeed0000u+i;
}
static void _mesa_upload_state_parameters(struct gl_context *ctx,
    struct gl_program_parameter_list *params, uint32_t *dest)
{
    (void)ctx;
    if (!dest) { ++tracker.invalid_state; return; }
    ++tracker.uploaded_state; write_state(params,dest);
}
static void _mesa_load_state_parameters(struct gl_context *ctx, struct gl_program_parameter_list *params)
{
    (void)ctx;
    ++tracker.loaded_state; write_state(params,(uint32_t*)params->ParameterValues);
}
