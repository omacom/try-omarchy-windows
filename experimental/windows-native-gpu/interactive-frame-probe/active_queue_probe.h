// SPDX-License-Identifier: MIT
// Bounded actual-compositor test: GPU-only ordinary frames, two audit copies.
#pragma once
#include <time.h>
#include "guest_trace_line.h"
#ifndef HYPRLAND_QUEUE_FRAMES
#define HYPRLAND_QUEUE_FRAMES 12u
#endif
#ifndef HYPRLAND_QUEUE_END_STREAM
#define HYPRLAND_QUEUE_END_STREAM 0
#endif
#if HYPRLAND_QUEUE_END_STREAM != 0 && HYPRLAND_QUEUE_END_STREAM != 1
#error End-of-stream diagnostic selection must be zero or one
#endif
#if HYPRLAND_QUEUE_END_STREAM != 1
#error Interactive diagnostics require an acknowledged end-of-stream frame
#endif
#if HYPRLAND_QUEUE_FRAMES < 2 || HYPRLAND_QUEUE_FRAMES > 1800
#error Queued compositor diagnostic must select 2..1800 frames
#endif
static uint64_t active_monotonic_ns(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
#define HYPRLAND_INTERACTIVE_MAX_FRAMES 6000u
static _Atomic int stream_finish_requested,stream_finished,stream_limit_hit;
static _Atomic unsigned queued_frames;
static _Atomic int queue_failed;
static _Atomic int failure_drain_requested,failure_drain_done;
static int queue_output(int bufferFd,uint32_t width,uint32_t height,uint32_t format,uint32_t stride) {
    const unsigned expected=atomic_load(&queued_frames)+1;
    const int failure_drain=atomic_load(&queue_failed) && atomic_load(&failure_drain_requested) && !atomic_load(&failure_drain_done);
    if (atomic_load(&stream_finished) || expected>HYPRLAND_INTERACTIVE_MAX_FRAMES || (atomic_load(&queue_failed) && !failure_drain)) return 0;
    const int capped=expected==HYPRLAND_INTERACTIVE_MAX_FRAMES;
    // Capture authorization before submitting. A finish request arriving while
    // the cap frame is in flight must not retroactively accept that frame.
    const int requested_finish=expected>=HYPRLAND_QUEUE_FRAMES && atomic_load(&stream_finish_requested);
    const int final_frame=failure_drain || capped || requested_finish;
    const uint64_t active_entered=active_monotonic_ns();
    if(!active_entered) goto failure;
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
    uint32_t flags=(expected==1 || final_frame) ? 1u : 0u;
#if HYPRLAND_QUEUE_END_STREAM
    if(final_frame) flags|=2u;
#endif
    uint64_t frame=0; int result=-1; unsigned retries=0;
    struct timespec start,now;
    if (clock_gettime(CLOCK_MONOTONIC,&start)) { close(fd); destroy(display,fence); goto failure; }
    const uint64_t active_queued=active_monotonic_ns();
    do {
        result=queue(bufferFd,fd,width,height,format,stride,flags,&frame);
        if (!result || errno!=EAGAIN) break;
        ++retries;
        if (clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec-start.tv_sec>=5) break;
        usleep(1000);
    } while (1);
    const uint64_t active_returned=active_monotonic_ns();
    const int error=errno;
    const int closed=close(fd);
    const unsigned destroyed=destroy(display,fence);
    if(guest_trace_line("HYPRLAND_HOST_QUEUE_RESULT result=%d errno=%d frame=%llu audit=%u retries=%u syncFileClosed=%s eglSyncDestroyed=%s guestCpuMap=false sourceCopyRetired=%s displayVerified=false\n",
        result,result?error:0,(unsigned long long)frame,flags&1u,retries,closed?"false":"true",destroyed?"true":"false",result?"false":"true")) goto failure;
    if (result || frame!=expected || closed || !destroyed) goto failure;
    if((flags&2u) && guest_trace_line("HYPRLAND_HOST_QUEUE_END_ACK frame=%llu outputDrained=true\n",(unsigned long long)frame)) goto failure;
    const uint64_t active_finished=active_monotonic_ns();
    if(!active_queued || active_queued<active_entered || active_returned<active_queued || active_finished<active_returned) goto failure;
    if(guest_trace_line("HYPRLAND_ACTIVE_QUEUE_TIMING frame=%llu audit=%u fenceSetupNs=%llu queueRoundTripNs=%llu totalNs=%llu queueAttempts=%u monotonicNs=%llu cpuWallClock=true gpuTimestamp=false\n",
        (unsigned long long)frame,flags&1u,(unsigned long long)(active_queued-active_entered),
        (unsigned long long)(active_returned-active_queued),(unsigned long long)(active_finished-active_entered),
        retries+1,(unsigned long long)active_finished)) goto failure;
    atomic_store(&queued_frames,expected);
    if(capped && !requested_finish) {
        atomic_store(&stream_limit_hit,1);atomic_store(&queue_failed,1);
    }
    // Publish completion only after publishing any cap failure to the observer.
    if(final_frame) atomic_store(&stream_finished,1);
    if(failure_drain) atomic_store(&failure_drain_done,1);
    return 0;
failure:
    atomic_store(&queue_failed,1);
    (void)guest_trace_line("HYPRLAND_HOST_QUEUE_FAILURE displayVerified=false\n");
    return -1;
}
