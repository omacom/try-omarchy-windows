/* CPU-only Gallium callback model. Resource records remain tombstones so a
 * double release is observed without dereferencing freed memory or crashing. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef unsigned uint;
enum pipe_resource_usage { PIPE_USAGE_STREAM };
enum { PIPE_BUFFER=1,PIPE_FORMAT_R8_UNORM=1,PIPE_BIND_VERTEX_BUFFER=1,
       PIPE_BIND_INDEX_BUFFER=2,PIPE_BIND_CONSTANT_BUFFER=4,
       PIPE_MAP_WRITE=1,PIPE_MAP_UNSYNCHRONIZED=2,PIPE_MAP_PERSISTENT=4,
       PIPE_MAP_COHERENT=8,PIPE_MAP_FLUSH_EXPLICIT=16,
       PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE=1,
       PIPE_RESOURCE_FLAG_MAP_PERSISTENT=2,PIPE_RESOURCE_FLAG_MAP_COHERENT=4 };
#define MESA_DEBUG 0
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define align(n,a) (((n)+(a)-1)&~((a)-1))
#define unlikely(value) (value)
#define CALLOC_STRUCT(name) ((struct name*)calloc(1,sizeof(struct name)))
#define FREE free
struct pipe_resource {
    unsigned target,format,bind,usage,flags,width0,height0,depth0,array_size;
    unsigned id,refs;
};
struct pipe_box { int x; unsigned width; };
struct pipe_transfer { struct pipe_box box; unsigned owner; bool active; };
struct pipe_screen {
    struct { bool buffer_map_persistent_coherent; } caps;
    struct pipe_resource* (*resource_create)(struct pipe_screen*,const struct pipe_resource*);
};
struct pipe_context { struct pipe_screen *screen; };
struct Record { struct pipe_resource resource; unsigned char bytes[8192]; struct pipe_transfer transfer; };
static struct {
    struct Record records[16]; unsigned used,double_release,invalid_map,model_error;
    bool fail_create,fail_map;
} model;
static struct pipe_resource* create_resource(struct pipe_screen *screen,const struct pipe_resource *desc)
{
    (void)screen;
    if(model.fail_create) { model.fail_create=false;return NULL; }
    if(model.used>=16 || desc->width0>8192) { ++model.model_error;return NULL; }
    struct Record *record=&model.records[model.used++];
    record->resource=*desc;record->resource.id=model.used;record->resource.refs=1;
    return &record->resource;
}
static void pipe_resource_release(struct pipe_context *pipe,struct pipe_resource *resource)
{
    (void)pipe;
    if(!resource)return;
    if(!resource->refs) { ++model.double_release;return; }
    --resource->refs;
}
static void pipe_resource_reference(struct pipe_resource **out,struct pipe_resource *resource)
{
    if(*out==resource)return;
    if(resource) { if(!resource->refs)++model.model_error;else ++resource->refs; }
    pipe_resource_release(NULL,*out);*out=resource;
}
static void* pipe_buffer_map_range(struct pipe_context *pipe,struct pipe_resource *resource,
    unsigned offset,unsigned bytes,unsigned flags,struct pipe_transfer **transfer)
{
    (void)pipe;(void)flags;*transfer=NULL;
    if(!resource || !resource->refs) { ++model.invalid_map;return NULL; }
    if(model.fail_map) { model.fail_map=false;return NULL; }
    if(offset>resource->width0 || bytes>resource->width0-offset) { ++model.model_error;return NULL; }
    struct Record *record=&model.records[resource->id-1];
    record->transfer.box.x=(int)offset;record->transfer.box.width=bytes;
    record->transfer.owner=resource->id;record->transfer.active=true;
    *transfer=&record->transfer;return record->bytes+offset;
}
static void pipe_buffer_flush_mapped_range(struct pipe_context *pipe,struct pipe_transfer *transfer,unsigned offset,unsigned bytes)
{
    (void)pipe;
    if(!transfer->active || offset>(unsigned)transfer->box.x+transfer->box.width ||
       bytes>(unsigned)transfer->box.x+transfer->box.width-offset)++model.model_error;
}
static void pipe_buffer_unmap(struct pipe_context *pipe,struct pipe_transfer *transfer)
{
    (void)pipe;
    if(!transfer->active)++model.model_error;
    transfer->active=false;
}
struct u_upload_mgr;
void u_upload_disable_persistent(struct u_upload_mgr *upload);
