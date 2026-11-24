/* Explicit return checks remain active when the upstream assertions are off. */
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"fixture line %u\n",(unsigned)__LINE__); return 1; } } while (0)
struct fixture {
    struct pipe_screen screen;
    struct pipe_context pipe;
    struct gl_context ctx;
    struct st_context st;
    gl_constant_value values[8];
    struct gl_program_parameter parameters[2];
    struct gl_program_parameter_list params;
    struct gl_program prog;
};
static void init_fixture(struct fixture *f, unsigned stage, bool persistent, unsigned mode, bool real)
{
    memset(f,0,sizeof(*f)); memset(&model,0,sizeof model); memset(&tracker,0,sizeof tracker);
    f->screen.caps.buffer_map_persistent_coherent=persistent;
    f->screen.resource_create=create_resource;
    f->pipe.screen=&f->screen;
    f->pipe.set_constant_buffer=set_constant_buffer;
    f->pipe.set_inlinable_constants=set_inlinable_constants;
    f->pipe.const_uploader=u_upload_create(&f->pipe,64,PIPE_BIND_CONSTANT_BUFFER,PIPE_USAGE_STREAM,0);
    f->ctx.Const.UniformBufferOffsetAlignment=256;
    f->st.pipe=&f->pipe; f->st.ctx=&f->ctx; f->st.prefer_real_buffer_in_constbuf0=real;
    f->st.state.constbuf0_enabled_shader_mask=(1u<<stage)|(1u<<((stage+1)%8));
    f->params.NumParameters=2; f->params.NumParameterValues=8;
    f->params.UniformBytes=mode==1?0:16; f->params.StateFlags=mode!=0;
    for (unsigned i=0;i<8;++i) f->values[i].u=0x12340000u+i;
    f->parameters[1].ValueOffset=4;
    f->params.ParameterValues=f->values; f->params.Parameters=f->parameters;
    f->prog.Parameters=&f->params; f->prog.info.num_inlinable_uniforms=2;
    f->prog.info.inlinable_uniform_dw_offsets[0]=0;
    f->prog.info.inlinable_uniform_dw_offsets[1]=mode==2?4:1;
}
static unsigned live_resources(void)
{
    unsigned count=0; for(unsigned i=0;i<model.used;++i) count+=model.records[i].resource.refs!=0; return count;
}
static int finish(struct fixture *f)
{
    for(unsigned i=0;i<8;++i) pipe_resource_reference(&tracker.bound[i],NULL);
    drain_releasebufs(&f->st); u_upload_destroy(f->pipe.const_uploader);
    REQUIRE(!live_resources() && !model.double_release && !model.model_error && !model.invalid_map);
    return 0;
}
static int verify_success(struct fixture *f, unsigned stage, bool real)
{
    const uint32_t *data;
    REQUIRE(tracker.inlinable==1 && tracker.subroutine==1);
    REQUIRE(tracker.samplers==1 && tracker.images==1);
    REQUIRE(tracker.bind==1 && !tracker.invalid_bind && !tracker.errors);
    REQUIRE(!tracker.invalid_copy && !tracker.invalid_state);
    REQUIRE(f->st.state.constbuf0_enabled_shader_mask & (1u<<stage));
    REQUIRE(tracker.size[stage]==32);
    if(real) {
        REQUIRE(tracker.bound[stage] && !tracker.user[stage]);
        REQUIRE(tracker.offset[stage]%256==0);
        data=(uint32_t*)(model.records[tracker.bound[stage]->id-1].bytes+tracker.offset[stage]);
        REQUIRE(tracker.uploaded_state==(unsigned)(f->params.StateFlags!=0));
    } else {
        REQUIRE(!tracker.bound[stage] && tracker.user[stage]==f->values);
        data=(const uint32_t*)tracker.user[stage];
        REQUIRE(!tracker.uploaded_state);
    }
    if(f->params.UniformBytes) REQUIRE(!memcmp(data,f->values,(unsigned)f->params.UniformBytes));
    if(f->params.StateFlags) {
        unsigned start=(unsigned)f->params.UniformBytes/4;
        for(unsigned i=0;i<4;++i) REQUIRE(data[start+i]==0xfeed0000u+i);
    }
    for(unsigned i=0;i<2;++i) REQUIRE(tracker.inline_values[i]==f->values[f->prog.info.inlinable_uniform_dw_offsets[i]].u);
    return 0;
}
static int fail_case(unsigned stage, bool persistent, unsigned scenario, unsigned mode)
{
    struct fixture f; init_fixture(&f,stage,persistent,mode,true);
    REQUIRE(f.pipe.const_uploader);
    struct pipe_resource *buffer=NULL,*release=NULL;
    unsigned offset=0; void *ptr=NULL;
    if(scenario>=2) {
        u_upload_alloc(f.pipe.const_uploader,0,16,1,&offset,&buffer,&release,&ptr);
        REQUIRE(ptr && buffer && !release);
        struct pipe_constant_buffer cb={buffer,NULL,offset,16};
        set_constant_buffer(&f.pipe,stage,0,&cb);
        if(scenario==4) u_upload_unmap(f.pipe.const_uploader);
        else {
            unsigned remaining=f.pipe.const_uploader->buffer_size-f.pipe.const_uploader->offset;
            u_upload_alloc(f.pipe.const_uploader,0,remaining,1,&offset,&buffer,&release,&ptr);
            REQUIRE(ptr && !release);
        }
    }
    tracker.bind=0;
    if(scenario==0 || scenario==2) model.fail_create=true; else model.fail_map=true;
    st_upload_constants(&f.st,&f.prog,stage);
#if EXPECT_GUARDED
    REQUIRE(tracker.errors==1 && tracker.unbind==1 && !tracker.bind);
    REQUIRE(!tracker.invalid_copy && !tracker.invalid_state && !tracker.invalid_bind);
    REQUIRE(!tracker.copies && !tracker.uploaded_state && !tracker.loaded_state && !tracker.inlinable);
    REQUIRE(f.st.deferred_count==(unsigned)(scenario==2 || scenario==3));
    REQUIRE(f.st.state.constbuf0_enabled_shader_mask==(1u<<((stage+1)%8)));
    REQUIRE(!tracker.bound[stage]);
    unsigned errors=tracker.errors;
    /* Retry after a single transient CPU callback failure. This is not GPU recovery. */
    memset(&tracker,0,sizeof tracker);
    st_upload_constants(&f.st,&f.prog,stage);
    REQUIRE(!verify_success(&f,stage,true));
    printf("{\"fault\":true,\"stage\":%u,\"persistent\":%s,\"scenario\":%u,\"mode\":%u,\"guarded\":true,\"errors\":%u,\"retry\":true}\n",
        stage,persistent?"true":"false",scenario,mode,errors);
#else
    REQUIRE(!tracker.errors && !tracker.unbind && tracker.bind==1 && tracker.invalid_bind==1);
    REQUIRE(tracker.invalid_copy==(unsigned)(mode!=1) && tracker.invalid_state==(unsigned)(mode!=0));
    printf("{\"fault\":true,\"stage\":%u,\"persistent\":%s,\"scenario\":%u,\"mode\":%u,\"guarded\":false,\"invalidCopy\":%u,\"invalidState\":%u,\"invalidBind\":%u}\n",
        stage,persistent?"true":"false",scenario,mode,tracker.invalid_copy,tracker.invalid_state,tracker.invalid_bind);
#endif
    return finish(&f);
}
static int success_case(unsigned stage, bool persistent, unsigned mode, bool real)
{
    struct fixture f; init_fixture(&f,stage,persistent,mode,real);
    REQUIRE(f.pipe.const_uploader);
    st_upload_constants(&f.st,&f.prog,stage);
    REQUIRE(!verify_success(&f,stage,real));
    printf("{\"positive\":true,\"stage\":%u,\"persistent\":%s,\"mode\":%u,\"real\":%s}\n",
        stage,persistent?"true":"false",mode,real?"true":"false");
    return finish(&f);
}
static int empty_case(unsigned stage, unsigned type)
{
    struct fixture f; init_fixture(&f,stage,false,0,true);
    if(type==1) f.prog.Parameters=NULL;
    if(type==2) f.params.NumParameters=0;
    st_upload_constants(&f.st,type==0?NULL:&f.prog,stage);
    REQUIRE(tracker.unbind==1 && !tracker.bind && !tracker.errors && !model.used);
    REQUIRE(f.st.state.constbuf0_enabled_shader_mask==(1u<<((stage+1)%8)));
    printf("{\"empty\":true,\"stage\":%u,\"type\":%u}\n",stage,type);
    return finish(&f);
}
static int ati_case(bool persistent)
{
    struct fixture f; init_fixture(&f,MESA_SHADER_FRAGMENT,persistent,0,true);
    struct ati_fragment_shader ati={0};
    ati.LocalConstDef=1;
    for(unsigned i=0;i<4;++i) { ati.Constants[0][i]=(float)(i+1); f.ctx.ATIFragmentShader.GlobalConstants[1][i]=(float)(i+5); }
    f.prog.ati_fs=&ati; f.params.UniformBytes=32;
    st_upload_constants(&f.st,&f.prog,MESA_SHADER_FRAGMENT);
    REQUIRE(!verify_success(&f,MESA_SHADER_FRAGMENT,true));
    REQUIRE(!memcmp(f.values,ati.Constants[0],16));
    REQUIRE(!memcmp(f.values+4,f.ctx.ATIFragmentShader.GlobalConstants[1],16));
    printf("{\"ati\":true,\"persistent\":%s}\n",persistent?"true":"false");
    return finish(&f);
}
int main(void)
{
    for(unsigned stage=0;stage<8;++stage) {
        for(unsigned persistent=0;persistent<2;++persistent) {
            for(unsigned mode=0;mode<3;++mode) {
                for(unsigned scenario=0;scenario<(persistent?4u:5u);++scenario)
                    if(fail_case(stage,persistent!=0,scenario,mode)) return 10;
                for(unsigned real=0;real<2;++real)
                    if(success_case(stage,persistent!=0,mode,real!=0)) return 11;
            }
        }
        for(unsigned type=0;type<3;++type) if(empty_case(stage,type)) return 12;
    }
    for(unsigned persistent=0;persistent<2;++persistent) if(ati_case(persistent!=0)) return 13;
    return 0;
}
