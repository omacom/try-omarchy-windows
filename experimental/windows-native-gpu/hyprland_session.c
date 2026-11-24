// SPDX-License-Identifier: MIT
// Opt-in persistent Hyprland output. No fixture, forced repaint or lifetime timer.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "hyprland_session_invocation.h"

#ifndef WDDM_SESSION_COMPOSITOR_PATH
#define WDDM_SESSION_COMPOSITOR_PATH "/usr/bin/Hyprland"
#endif

typedef int (*session_main_function)(int,char**,char**);
static session_main_function original_main;
static _Atomic int session_active,session_failed;
static atomic_flag committing=ATOMIC_FLAG_INIT;
static uint32_t session_width=1920,session_height=1080,session_format;
static uint64_t session_frames;
static unsigned startup_buffer_deferred;
static int (*native_info)(int,uint64_t*);
static int (*native_queue)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*);
static void* (*current_display)(void);
static void* (*get_proc)(const char*);
static void (*flush_commands)(void);
static void* (*create_sync)(void*,unsigned,const int*);
static unsigned (*destroy_sync)(void*,void*);
static int (*duplicate_sync)(void*,void*);

// Journald supplies a stream socket, which cannot be reopened through /proc.
// Use per-send nonblocking flags there; reopen pipes/files with a separate file
// description. Never change the parent's flags or wait for a full sink.
static void session_diagnostic(const char* line,int bytes) {
    const int saved=errno;
    if(bytes>0 && bytes<256) {
        int type=0;socklen_t size=sizeof type;
        if(!getsockopt(STDERR_FILENO,SOL_SOCKET,SO_TYPE,&type,&size) && size==sizeof type) {
            const ssize_t ignored=send(STDERR_FILENO,line,(size_t)bytes,MSG_DONTWAIT|MSG_NOSIGNAL);(void)ignored;
        } else {
            const int fd=open("/proc/self/fd/2",O_WRONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY|O_APPEND);
            if(fd>=0) {
                const ssize_t ignored=write(fd,line,(size_t)bytes);(void)ignored;
                const int closed=close(fd);(void)closed;
            }
        }
    }
    errno=saved;
}
static int fail_session(int error,const char* stage) {
    if(!atomic_exchange(&session_failed,1)) {
        char line[256];
        const int bytes=snprintf(line,sizeof line,"HYPRLAND_SESSION_FAILURE errno=%d softwareFallback=false stage=%s\n",error,stage);
        session_diagnostic(line,bytes);
        // Hyprland owns its SIGTERM event source. Request ordinary shutdown
        // only after our duplicated fence and EGL sync have been released.
        const int requested=kill(getpid(),SIGTERM);(void)requested;
    }
    errno=error;return -1;
}
static uint64_t monotonic_ns(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
// Used by the Aquamarine session adapter on its existing event-loop thread.
int wddm_bridge_compositor_session_mode(uint32_t* width,uint32_t* height) {
    if(!atomic_load(&session_active)) return 0;
    if(!width || !height || atomic_load(&session_failed)) { errno=EINVAL;return -1; }
    *width=session_width;*height=session_height;return 1;
}
int wddm_bridge_compositor_commit(int buffer_fd,uint32_t width,uint32_t height,
                                 uint32_t format,uint32_t stride,uint64_t modifier) {
    (void)modifier; // Ownership/byte layout are independently checked by the bridge.
    if(!atomic_load(&session_active)) return 0;
    if(atomic_load(&session_failed)) { errno=EIO;return -1; }
    if(atomic_flag_test_and_set(&committing)) { errno=EBUSY;return -1; }
    int result=-1,error=EINVAL,fence_fd=-1;void* display=NULL;void* fence=NULL;
    const char* stage="buffer-layout";
    uint64_t bytes=0,frame=0;
    if(buffer_fd<0 || width!=session_width || height!=session_height || stride!=width*4 ||
       (format!=0x34325241u && format!=0x34325258u && format!=0x34324241u && format!=0x34324258u) ||
       (session_format && format!=session_format)) goto done;
    stage="frame-sequence";
    if(session_frames>=UINT64_MAX-1) { error=EOVERFLOW;goto done; }
    stage="buffer-info";
    if(native_info(buffer_fd,&bytes)) { error=errno?errno:EIO;goto done; }
    stage="buffer-bytes";
    if(bytes!=(uint64_t)stride*height) goto done;
    stage="egl-entry-points";
    if(!current_display) {
        current_display=(void* (*)(void))dlsym(RTLD_DEFAULT,"eglGetCurrentDisplay");
        get_proc=(void* (*)(const char*))dlsym(RTLD_DEFAULT,"eglGetProcAddress");
        flush_commands=(void (*)(void))dlsym(RTLD_DEFAULT,"glFlush");
        if(!current_display || !get_proc || !flush_commands) { error=ENOSYS;goto done; }
        create_sync=(void* (*)(void*,unsigned,const int*))get_proc("eglCreateSyncKHR");
        destroy_sync=(unsigned (*)(void*,void*))get_proc("eglDestroySyncKHR");
        duplicate_sync=(int (*)(void*,void*))get_proc("eglDupNativeFenceFDANDROID");
    }
    if(!create_sync || !destroy_sync || !duplicate_sync) { error=ENOSYS;goto done; }
    stage="egl-current-display";
    display=current_display();
    if(!display) {
        // Hyprland commits its initial output buffer before making its render
        // context current. Consume only that validated startup state; it has no
        // rendered content/fence and must never reach the Windows frame queue.
        if(!session_frames && !startup_buffer_deferred) {
            startup_buffer_deferred=1;
            static const char line[]="HYPRLAND_SESSION_STARTUP_BUFFER deferred=true reason=no-current-egl-display nativeBufferValidated=true renderedFrame=false\n";
            session_diagnostic(line,(int)sizeof line-1);
            result=0;error=0;goto done;
        }
        error=ENODEV;goto done;
    }
    stage="egl-create-sync";
    fence=create_sync(display,0x3144,NULL);
    if(!fence) { error=EIO;goto done; }
    flush_commands();
    stage="egl-duplicate-sync";
    fence_fd=duplicate_sync(display,fence);
    if(fence_fd<0) { error=errno?errno:EIO;goto done; }
    stage="queue-clock";
    const uint64_t start=monotonic_ns();
    if(!start || start>UINT64_MAX-5000000000ull) { error=EIO;goto done; }
    const uint64_t deadline=start+5000000000ull;
    for(;;) {
        stage="native-queue";
        // Zero flags: no audit readback, finite-frame sentinel or END command.
        result=native_queue(buffer_fd,fence_fd,width,height,format,stride,0,&frame);
        if(!result) break;
        error=errno?errno:EIO;
        if(error!=EAGAIN) goto done;
        const uint64_t now=monotonic_ns();
        if(!now || now<start) { error=EIO;goto done; }
        if(now>=deadline) { error=ETIMEDOUT;goto done; }
        const struct timespec delay={.tv_sec=0,.tv_nsec=1000000};
        if(nanosleep(&delay,NULL) && errno!=EINTR) { error=errno;goto done; }
        const uint64_t after_sleep=monotonic_ns();
        if(!after_sleep || after_sleep<now) { error=EIO;goto done; }
        if(after_sleep>=deadline) { error=ETIMEDOUT;goto done; }
    }
    stage="queue-sequence";
    if(frame!=session_frames+1) { result=-1;error=EPROTO;goto done; }
    session_frames=frame;session_format=format;
    error=0;
done:
    if(fence_fd>=0 && close(fence_fd)) { if(!error) { error=errno?errno:EIO;stage="close-fence-fd"; }result=-1; }
    if(fence && !destroy_sync(display,fence)) { if(!error) { error=EIO;stage="destroy-egl-sync"; }result=-1; }
    if(error) result=fail_session(error,stage);
    atomic_flag_clear(&committing);
    return result;
}
static int session_main(int argc,char** argv,char** environment) {
    const char* selected=getenv("WDDM_BRIDGE_COMPOSITOR_SESSION");
    if(!selected) return original_main(argc,argv,environment);
    char executable[4096];
    const ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(length<0 || (size_t)length>=sizeof(executable)-1) return 125;
    executable[length]=0;
    if(strcmp(executable,WDDM_SESSION_COMPOSITOR_PATH))
        return original_main(argc,argv,environment);
    if(!wddm_hyprland_renders(argc,argv)) return original_main(argc,argv,environment);
    if(strcmp(selected,"1") || !getuid() || getuid()!=geteuid()) return 125;
    const char* size=getenv("WDDM_BRIDGE_SESSION_SIZE");
    if(size && strcmp(size,"1920x1080")) {
        if(strcmp(size,"3840x2160")) return 125;
        session_width=3840;session_height=2160;
    }
    if(getenv("AQ_EXPERIMENTAL_NATIVE_CAPTURE")) return 125;
    const char* sync=getenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE");
    const char* buffers=getenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS");
    if(!sync || strcmp(sync,"true") || !buffers || strcmp(buffers,"true")) return 125;
    int (*begin)(void)=(int (*)(void))dlsym(RTLD_DEFAULT,"wddm_bridge_begin_opengl_multiapp_render");
    native_info=(int (*)(int,uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info");
    native_queue=(int (*)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*))
        dlsym(RTLD_DEFAULT,"wddm_bridge_queue_compositor_buffer");
    if(!begin || !native_info || !native_queue || begin()) return 125;
    atomic_store(&session_active,1);
    char line[256];
    int bytes=snprintf(line,sizeof line,"HYPRLAND_SESSION_BEGIN profile=14 width=%u height=%u argvPreserved=true finiteWorkload=false\n",session_width,session_height);
    session_diagnostic(line,bytes);
    const int result=original_main(argc,argv,environment);
    atomic_store(&session_active,0);
    bytes=snprintf(line,sizeof line,"HYPRLAND_SESSION_RETURN exit=%d frames=%llu failed=%s\n",result,
        (unsigned long long)session_frames,atomic_load(&session_failed)?"true":"false");
    session_diagnostic(line,bytes);
    return atomic_load(&session_failed)?125:result;
}
int __libc_start_main(session_main_function main,int argc,char** argv,void (*init)(void),
    void (*fini)(void),void (*rtld_fini)(void),void* stack_end) {
    typedef int (*start_function)(session_main_function,int,char**,void (*)(void),void (*)(void),void (*)(void),void*);
    start_function original=(start_function)dlsym(RTLD_NEXT,"__libc_start_main");
    if(!original) _exit(125);
    original_main=main;
    return original(session_main,argc,argv,init,fini,rtld_fini,stack_end);
}
