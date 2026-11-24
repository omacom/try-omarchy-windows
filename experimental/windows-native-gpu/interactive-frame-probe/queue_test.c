/* SPDX-License-Identifier: MIT */
/* Exercise the real queue hook with mock EGL/native work. Opens no UI or GPU. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static void* mock_dlsym(void*,const char*);
static int mock_close(int);
static ssize_t mock_write(int,const void*,size_t);
#define dlsym mock_dlsym
#define close mock_close
#define write mock_write
#include "active_queue_probe.h"
#undef write
#undef close
#undef dlsym

static unsigned native_frames,queue_calls,closes,destroys,flushes,end_acks,writes;
static unsigned observed_flags[6001];
static int native_error,close_error,destroy_error,missing_queue,late_finish,write_error;
static void* mock_display(void) { return (void*)(uintptr_t)1; }
static void mock_flush(void) { ++flushes; }
static void* mock_create(void* display,unsigned kind,const int* attributes) {
    assert(display==(void*)(uintptr_t)1 && kind==0x3144 && !attributes);
    return (void*)(uintptr_t)2;
}
static unsigned mock_destroy(void* display,void* fence) {
    assert(display==(void*)(uintptr_t)1 && fence==(void*)(uintptr_t)2);
    ++destroys;return !destroy_error;
}
static int mock_duplicate(void* display,void* fence) {
    assert(display==(void*)(uintptr_t)1 && fence==(void*)(uintptr_t)2);return 7;
}
static int mock_queue(int buffer,int fence,uint32_t width,uint32_t height,
                      uint32_t format,uint32_t stride,uint32_t flags,uint64_t* frame) {
    assert(buffer==19 && fence==7 && width==3840 && height==2160 && format==42 && stride==15360);
    assert(flags<=3);++queue_calls;
    if(native_error) { errno=EIO;return -1; }
    assert(native_frames<6000);observed_flags[++native_frames]=flags;*frame=native_frames;
    if(late_finish && native_frames==6000) atomic_store(&stream_finish_requested,1);
    return 0;
}
static void* mock_proc(const char* name) {
    if(!strcmp(name,"eglCreateSyncKHR")) return (void*)mock_create;
    if(!strcmp(name,"eglDestroySyncKHR")) return (void*)mock_destroy;
    if(!strcmp(name,"eglDupNativeFenceFDANDROID")) return (void*)mock_duplicate;
    assert(0);return NULL;
}
static void* mock_dlsym(void* handle,const char* name) {
    assert(handle==RTLD_DEFAULT);
    if(!strcmp(name,"eglGetCurrentDisplay")) return (void*)mock_display;
    if(!strcmp(name,"eglGetProcAddress")) return (void*)mock_proc;
    if(!strcmp(name,"glFlush")) return (void*)mock_flush;
    if(!strcmp(name,"wddm_bridge_queue_compositor_buffer")) return missing_queue ? NULL : (void*)mock_queue;
    assert(0);return NULL;
}
static int mock_close(int fd) { assert(fd==7);++closes;return close_error ? -1 : 0; }
static ssize_t mock_write(int fd,const void* data,size_t count) {
    assert(fd==STDOUT_FILENO && count>2 && count<=512);
    const char* line=data;assert(line[0]=='\n' && line[count-1]=='\n');
    assert(!memchr(line+1,'\n',count-2));++writes;
    if(write_error && writes==(unsigned)write_error) { errno=EIO;return -1; }
    static const char end_prefix[]="HYPRLAND_HOST_QUEUE_END_ACK ";
    if(count>sizeof end_prefix && !memcmp(line+1,end_prefix,sizeof end_prefix-1)) ++end_acks;
    return (ssize_t)count;
}
static void reset(void) {
    atomic_store(&queued_frames,0);atomic_store(&queue_failed,0);
    atomic_store(&failure_drain_requested,0);atomic_store(&failure_drain_done,0);
    atomic_store(&stream_finish_requested,0);atomic_store(&stream_finished,0);atomic_store(&stream_limit_hit,0);
    native_frames=queue_calls=closes=destroys=flushes=end_acks=writes=0;
    native_error=close_error=destroy_error=missing_queue=late_finish=write_error=0;
    memset(observed_flags,0,sizeof observed_flags);
}
static int output(void) { return queue_output(19,3840,2160,42,15360); }
static void send_frames(unsigned count) { for(unsigned n=0;n<count;++n) assert(!output()); }
static void only_boundary_audits(unsigned final) {
    assert(observed_flags[1]==1 && observed_flags[final]==3);
    for(unsigned n=2;n<final;++n) assert(observed_flags[n]==0);
    assert(end_acks==1 && closes==native_frames && destroys==native_frames && flushes==native_frames);
}
int main(void) {
    reset();send_frames(2000);
    assert(atomic_load(&queued_frames)==2000 && !atomic_load(&stream_finished));
    assert(!observed_flags[1799] && !observed_flags[1800] && !end_acks);
    atomic_store(&stream_finish_requested,1);assert(!output());
    assert(atomic_load(&stream_finished) && !atomic_load(&queue_failed) && !atomic_load(&stream_limit_hit));
    only_boundary_audits(2001);assert(!output() && native_frames==2001);

    reset();atomic_store(&stream_finish_requested,1);send_frames(1799);
    assert(!atomic_load(&stream_finished));assert(!output());
    assert(atomic_load(&stream_finished) && !atomic_load(&queue_failed));only_boundary_audits(1800);

    reset();send_frames(6000);
    assert(atomic_load(&stream_finished) && atomic_load(&stream_limit_hit) && atomic_load(&queue_failed));
    only_boundary_audits(6000);assert(!output() && native_frames==6000);

    reset();send_frames(5999);atomic_store(&stream_finish_requested,1);assert(!output());
    assert(atomic_load(&stream_finished) && !atomic_load(&stream_limit_hit) && !atomic_load(&queue_failed));
    only_boundary_audits(6000);

    reset();late_finish=1;send_frames(6000);
    assert(atomic_load(&stream_finish_requested) && atomic_load(&stream_limit_hit) && atomic_load(&queue_failed));
    assert(atomic_load(&stream_finished));only_boundary_audits(6000);

    reset();send_frames(8);native_error=1;assert(output()==-1);
    assert(atomic_load(&queue_failed) && atomic_load(&queued_frames)==8 && !atomic_load(&stream_finished));
    unsigned calls=queue_calls;assert(!output() && queue_calls==calls);
    native_error=0;atomic_store(&failure_drain_requested,1);assert(!output());
    assert(atomic_load(&queued_frames)==9 && atomic_load(&stream_finished) && atomic_load(&failure_drain_done));
    assert(atomic_load(&queue_failed) && !atomic_load(&stream_limit_hit) && observed_flags[9]==3 && end_acks==1);
    assert(closes==10 && destroys==10);

    reset();missing_queue=1;assert(output()==-1 && !queue_calls && !closes && !destroys);
    assert(atomic_load(&queue_failed) && !atomic_load(&stream_finished));
    reset();close_error=1;assert(output()==-1 && closes==1 && destroys==1);
    assert(!atomic_load(&queued_frames) && !atomic_load(&stream_finished));
    reset();destroy_error=1;assert(output()==-1 && closes==1 && destroys==1);
    assert(!atomic_load(&queued_frames) && !atomic_load(&stream_finished));

    reset();atomic_store(&stream_finish_requested,1);send_frames(1799);
    write_error=(int)writes+2;assert(output()==-1);
    assert(native_frames==1800 && observed_flags[1800]==3 && !end_acks);
    assert(atomic_load(&queue_failed) && !atomic_load(&stream_finished) && atomic_load(&queued_frames)==1799);
    puts("INTERACTIVE_QUEUE mockGpu=true windowsOpened=0 continuePastMinimum=true finalAckRequired=true capRejected=true lateFinishRejected=true failurePreserved=true resourceFaultsRejected=true");
}
