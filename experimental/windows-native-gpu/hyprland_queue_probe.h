// SPDX-License-Identifier: MIT
// Bounded actual-compositor test: GPU-only ordinary frames, two audit copies.
#pragma once
#include <time.h>
#ifndef HYPRLAND_QUEUE_FRAMES
#define HYPRLAND_QUEUE_FRAMES 12u
#endif
#ifndef HYPRLAND_QUEUE_END_STREAM
#define HYPRLAND_QUEUE_END_STREAM 0
#endif
#if HYPRLAND_QUEUE_END_STREAM != 0 && HYPRLAND_QUEUE_END_STREAM != 1
#error End-of-stream diagnostic selection must be zero or one
#endif
#if HYPRLAND_QUEUE_FRAMES < 2 || HYPRLAND_QUEUE_FRAMES > 1800
#error Queued compositor diagnostic must select 2..1800 frames
#endif
static _Atomic unsigned queued_frames;
static _Atomic int queue_failed;
static int queue_output(int bufferFd,uint32_t width,uint32_t height,uint32_t format,uint32_t stride) {
    const unsigned expected=atomic_load(&queued_frames)+1;
    if (expected>HYPRLAND_QUEUE_FRAMES || atomic_load(&queue_failed)) return 0;
    void* (*currentDisplay)(void)=(void* (*)(void))dlsym(RTLD_DEFAULT,"eglGetCurrentDisplay");
    void* (*proc)(const char*)=(void* (*)(const char*))dlsym(RTLD_DEFAULT,"eglGetProcAddress");
    void (*flush)(void)=(void (*)(void))dlsym(RTLD_DEFAULT,"glFlush");
    int (*queue)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*)=
        (int (*)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_queue_compositor_buffer");
    if (!currentDisplay || !proc || !flush || !queue) goto failure;
    void* display=currentDisplay();
    void* (*create)(void*,unsigned,const int*)=(void* (*)(void*,unsigned,const int*))proc("eglCreateSyncKHR");
    unsigned (*destroy)(void*,void*)=(unsigned (*)(void*,void*))proc("eglDestroySyncKHR");
    int (*duplicate)(void*,void*)=(int (*)(void*,void*))proc("eglDupNativeFenceFDANDROID");
    if (!display || !create || !destroy || !duplicate) goto failure;
    void* fence=create(display,0x3144,NULL);
    if (!fence) goto failure;
    flush();
    const int fd=duplicate(display,fence);
    if (fd<0) { destroy(display,fence); goto failure; }
    uint32_t flags=(expected==1 || expected==HYPRLAND_QUEUE_FRAMES) ? 1u : 0u;
#if HYPRLAND_QUEUE_END_STREAM
    if(expected==HYPRLAND_QUEUE_FRAMES) flags|=2u;
#endif
    uint64_t frame=0; int result=-1; unsigned retries=0;
    struct timespec start,now;
    if (clock_gettime(CLOCK_MONOTONIC,&start)) { close(fd); destroy(display,fence); goto failure; }
    do {
        result=queue(bufferFd,fd,width,height,format,stride,flags,&frame);
        if (!result || errno!=EAGAIN) break;
        ++retries;
        if (clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec-start.tv_sec>=5) break;
        usleep(1000);
    } while (1);
    const int error=errno;
    const int closed=close(fd);
    const unsigned destroyed=destroy(display,fence);
    printf("HYPRLAND_HOST_QUEUE_RESULT result=%d errno=%d frame=%llu audit=%u retries=%u syncFileClosed=%s eglSyncDestroyed=%s guestCpuMap=false sourceCopyRetired=%s displayVerified=false\n",
        result,result?error:0,(unsigned long long)frame,flags&1u,retries,closed?"false":"true",destroyed?"true":"false",result?"false":"true");
    if (result || frame!=expected || closed || !destroyed) goto failure;
    if(flags&2u) printf("HYPRLAND_HOST_QUEUE_END_ACK frame=%llu outputDrained=true\n",(unsigned long long)frame);
    atomic_store(&queued_frames,expected);
    return 0;
failure:
    atomic_store(&queue_failed,1);
    puts("HYPRLAND_HOST_QUEUE_FAILURE displayVerified=false");
    return -1;
}
