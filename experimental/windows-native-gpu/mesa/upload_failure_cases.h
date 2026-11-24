static unsigned live_resources(void)
{
    unsigned count=0;for(unsigned i=0;i<model.used;++i)count+=model.records[i].resource.refs!=0;return count;
}
static int run_case(unsigned scenario,bool persistent)
{
    memset(&model,0,sizeof model);
    struct pipe_screen screen={0};screen.caps.buffer_map_persistent_coherent=persistent;screen.resource_create=create_resource;
    struct pipe_context pipe={&screen};
    struct u_upload_mgr *upload=u_upload_create(&pipe,64,PIPE_BIND_CONSTANT_BUFFER,PIPE_USAGE_STREAM,0);
    if(!upload)return 1;
    struct pipe_resource *buffer=NULL,*release=NULL;
    unsigned offset=0;void *pointer=NULL;
    const bool replacing=scenario>=2;
    if(replacing) {
        u_upload_alloc(upload,0,16,1,&offset,&buffer,&release,&pointer);
        if(!pointer || !buffer || release)return 2;
        const unsigned remaining=upload->buffer_size-upload->offset;
        u_upload_alloc(upload,0,remaining,1,&offset,&buffer,&release,&pointer);
        if(!pointer || release || upload->offset!=upload->buffer_size)return 3;
    }
    if(scenario==0 || scenario==2)model.fail_create=true;else model.fail_map=true;
    pointer=(void*)(uintptr_t)1;buffer=NULL;release=NULL;
    u_upload_alloc(upload,0,16,1,&offset,&buffer,&release,&pointer);
    const bool failed=pointer==NULL && offset==~0u;
    const bool old_returned=release!=NULL;
    const bool dangling=upload->buffer && !upload->buffer->refs;
    pipe_resource_release(&pipe,release);
    u_upload_destroy(upload);
    printf("{\"scenario\":%u,\"persistent\":%s,\"failed\":%s,\"oldReturned\":%s,\"danglingOwner\":%s,\"doubleRelease\":%u,\"liveAfterDestroy\":%u,\"modelErrors\":%u}\n",
        scenario,persistent?"true":"false",failed?"true":"false",old_returned?"true":"false",dangling?"true":"false",model.double_release,live_resources(),model.model_error);
    return model.model_error || !failed;
}
static int positive(bool persistent)
{
    memset(&model,0,sizeof model);
    struct pipe_screen screen={0};screen.caps.buffer_map_persistent_coherent=persistent;screen.resource_create=create_resource;
    struct pipe_context pipe={&screen};
    struct u_upload_mgr *upload=u_upload_create(&pipe,64,PIPE_BIND_CONSTANT_BUFFER,PIPE_USAGE_STREAM,0);
    const unsigned char data[8]={1,2,3,4,5,6,7,8};
    struct pipe_resource *buffer=NULL;unsigned offset=0;
    u_upload_data_ref(upload,0,sizeof data,16,data,&offset,&buffer);
    if(!buffer || memcmp(model.records[buffer->id-1].bytes+offset,data,sizeof data))return 4;
    pipe_resource_reference(&buffer,NULL);u_upload_destroy(upload);
    printf("{\"positive\":true,\"persistent\":%s,\"doubleRelease\":%u,\"liveAfterDestroy\":%u,\"modelErrors\":%u}\n",
        persistent?"true":"false",model.double_release,live_resources(),model.model_error);
    return model.double_release || live_resources() || model.model_error;
}
int main(void)
{
    for(unsigned persistent=0;persistent<2;++persistent) {
        for(unsigned scenario=0;scenario<4;++scenario)if(run_case(scenario,persistent!=0))return 10;
        if(positive(persistent!=0))return 11;
    }
    return 0;
}
