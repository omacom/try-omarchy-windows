// SPDX-License-Identifier: MIT
// CPU-only compositor/bridge double. All GPU/EGL operations below are mocked.
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned begins,created,destroyed,flushed,queued,attempts,display_queries;
static int held_fence=-1,duplicate_fd=-1;
static const char* scenario;
static volatile sig_atomic_t shutdown_requests;
static void mock_shutdown(int signal_number) {
    assert(signal_number==SIGTERM && held_fence<0 && created==destroyed);
    ++shutdown_requests;
}

static int is_case(const char* name) { return scenario && !strcmp(scenario,name); }
int wddm_bridge_begin_opengl_multiapp_render(void) {
    scenario=getenv("SESSION_TEST_CASE");++begins;
    if(is_case("begin_failure")) { errno=EIO;return -1; }return 0;
}
int wddm_bridge_native_buffer_info(int fd,uint64_t* bytes) {
    struct stat st;if(fstat(fd,&st)) return -1;
    *bytes=(uint64_t)st.st_size+((is_case("wrong_bytes") || is_case("startup_wrong_bytes"))?1:0);return 0;
}
void* eglGetCurrentDisplay(void) {
    ++display_queries;
    return is_case("no_display") || is_case("startup_wrong_bytes") ||
        (is_case("startup_no_display") && display_queries==1) ||
        (is_case("late_no_display") && display_queries>1) ? NULL : (void*)(uintptr_t)1;
}
static void* mock_create(void* display,unsigned type,const int* attrs) {
    assert(display==(void*)(uintptr_t)1 && type==0x3144 && !attrs && held_fence<0);
    if(is_case("create_failure")) return NULL;
    held_fence=eventfd(1,EFD_CLOEXEC|EFD_NONBLOCK);assert(held_fence>=0);
    if(is_case("fd_zero")) { assert(held_fence==0);const int moved=fcntl(held_fence,F_DUPFD_CLOEXEC,3);assert(moved>=3 && !close(held_fence));held_fence=moved; }
    ++created;
    return (void*)(uintptr_t)2;
}
void glFlush(void) { assert(held_fence>=0);++flushed; }
static unsigned mock_destroy(void* display,void* sync) {
    assert(display==(void*)(uintptr_t)1 && sync==(void*)(uintptr_t)2 && held_fence>=0);
    assert(!close(held_fence));held_fence=-1;++destroyed;
    return !is_case("destroy_failure");
}
static int mock_duplicate(void* display,void* sync) {
    assert(display==(void*)(uintptr_t)1 && sync==(void*)(uintptr_t)2 && flushed==created);
    if(is_case("duplicate_failure")) { errno=EMFILE;return -1; }
    duplicate_fd=fcntl(held_fence,F_DUPFD_CLOEXEC,0);assert(duplicate_fd>=0);return duplicate_fd;
}
void* eglGetProcAddress(const char* name) {
    if(is_case("no_egl_entry")) return NULL;
    if(!strcmp(name,"eglCreateSyncKHR")) return (void*)mock_create;
    if(!strcmp(name,"eglDestroySyncKHR")) return (void*)mock_destroy;
    if(!strcmp(name,"eglDupNativeFenceFDANDROID")) return (void*)mock_duplicate;
    return NULL;
}
int wddm_bridge_queue_compositor_buffer(int buffer,int sync,uint32_t width,uint32_t height,
    uint32_t format,uint32_t stride,uint32_t flags,uint64_t* frame) {
    assert(fcntl(buffer,F_GETFD)>=0 && sync==duplicate_fd && fcntl(sync,F_GETFD)>=0);
    assert((width==1920 && height==1080) || (width==3840 && height==2160));
    assert(format==0x34325241u && stride==width*4 && flags==0 && frame && !*frame);
    if(is_case("fd_zero")) assert(sync==0);
    if(is_case("reentrant")) {
        int (*commit)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t)=
            (int (*)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t))dlsym(RTLD_DEFAULT,"wddm_bridge_compositor_commit");
        assert(commit && commit(buffer,width,height,format,stride,0)==-1 && errno==EBUSY);
    }
    ++attempts;
    if(is_case("retry") && attempts<=3) { errno=EAGAIN;return -1; }
    if(is_case("deadline")) { errno=EAGAIN;return -1; }
    if(is_case("queue_failure")) { errno=ENODEV;return -1; }
    ++queued;*frame=queued+(is_case("wrong_sequence")?1:0);return 0;
}
static unsigned fd_count(void) {
    DIR* dir=opendir("/proc/self/fd");assert(dir);unsigned count=0;
    for(struct dirent* entry;(entry=readdir(dir));) if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(!closedir(dir));return count;
}
int main(int argc,char** argv,char** environment) {
    (void)environment;
    if(getenv("SESSION_TEST_EXPECT_GROUPS")) {
        gid_t groups[2];
        assert(getgroups(2,groups)==2 &&
            ((groups[0]==65532 && groups[1]==65533) || (groups[1]==65532 && groups[0]==65533)));
    }
    const char* expected_home=getenv("SESSION_EXPECT_HOME");
    if(expected_home) assert(getenv("HOME") && !strcmp(getenv("HOME"),expected_home));
    const char* expected_gbm=getenv("SESSION_EXPECT_GBM_BACKENDS_PATH");
    if(expected_gbm) assert(getenv("GBM_BACKENDS_PATH") && !strcmp(getenv("GBM_BACKENDS_PATH"),expected_gbm));
    if(getenv("SESSION_EXPECT_D3D12_INTEROP")) {
        assert(getenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE") && !strcmp(getenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE"),"true"));
        assert(getenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS") && !strcmp(getenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS"),"true"));
    }
    if(getenv("SESSION_TEST_CLEAN_ENV"))
        assert(!getenv("EGL_PLATFORM") && !getenv("WDDM_BRIDGE_PORT") && !getenv("WDDM_BRIDGE_WINDOWS_WORKER"));
    const char* expected=getenv("SESSION_TEST_ARGUMENTS");
    if(expected) {
        for(int index=1;index<argc;++index) {
            const char* end=strchr(expected,'\n');
            assert(end && (size_t)(end-expected)==strlen(argv[index]) && !memcmp(expected,argv[index],(size_t)(end-expected)));
            expected=end+1;
        }
        assert(!*expected);
    } else {
        assert(argc==5 && !strcmp(argv[1],"--config") && !strcmp(argv[2],"/home/test/My settings.lua") &&
            !strcmp(argv[3],"--socket") && !strcmp(argv[4],"test socket"));
    }
    assert(getenv("SESSION_UNCHANGED") && !strcmp(getenv("SESSION_UNCHANGED"),"literal value with spaces"));
    int (*mode)(uint32_t*,uint32_t*)=(int (*)(uint32_t*,uint32_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_compositor_session_mode");
    int (*commit)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t)=
        (int (*)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t))dlsym(RTLD_DEFAULT,"wddm_bridge_compositor_commit");
    assert(mode && commit);
    uint32_t width=0,height=0;
    if(getenv("SESSION_TEST_QUERY")) {
        assert(!begins && !mode(&width,&height));
        puts("SESSION_QUERY_CONTROL nativeBegins=0 argvPreserved=true gpuMock=true");return 17;
    }
    const char* watchdog=getenv("SESSION_TEST_WATCHDOG_FD");
    if(watchdog) {
        const int fd=atoi(watchdog);
        assert(fd>=0 && fcntl(fd,F_GETFD)>=0 && write(fd,"W",1)==1);
    }
    if(!begins) { assert(!mode(&width,&height));puts("SESSION_CPU_CONTROL inactive argvPreserved=true gpuMock=true");return 23; }
    assert(begins==1 && mode(&width,&height)==1);
    const char* pause=getenv("SESSION_TEST_PAUSE_FD");
    if(pause) {
        const int fd=atoi(pause);char gate=0;
        printf("SESSION_EXEC_IDENTITY pid=%ld gpuMock=true\n",(long)getpid());fflush(stdout);
        assert(fd>=0 && read(fd,&gate,1)==1 && gate=='G');
    }
    unsigned before=fd_count();
    const int buffer=memfd_create("cpu-only-compositor-buffer",MFD_CLOEXEC);assert(buffer>=0);
    assert(!ftruncate(buffer,(off_t)width*height*4));
    if(is_case("fd_zero")) { assert(!close(0));--before; }
    const int expect_failure=!(is_case("roundtrip") || is_case("startup_no_display") || is_case("retry") || is_case("main_failure") || is_case("fd_zero") || is_case("reentrant"));
    if(expect_failure) {
        struct sigaction action={0};action.sa_handler=mock_shutdown;sigemptyset(&action.sa_mask);
        assert(!sigaction(SIGTERM,&action,NULL));
    }
    if(is_case("startup_no_display") || is_case("no_display")) {
        assert(!commit(buffer,width,height,0x34325241u,width*4,0));
        assert(!queued && !attempts && !created && !destroyed && held_fence<0 && !shutdown_requests);
        assert(fd_count()==before+1);
        puts("SESSION_STARTUP_BUFFER_CONTROL nativeQueueCalls=0 fencesCreated=0 renderedFrames=0");
    }
    if(is_case("late_no_display")) {
        assert(!commit(buffer,width,height,0x34325241u,width*4,0));
        assert(queued==1 && created==destroyed && held_fence<0 && !shutdown_requests);
    }
    const unsigned frames=(is_case("roundtrip") || is_case("startup_no_display"))?10000:1;
    for(unsigned n=0;n<frames;++n) {
        int fd=buffer;
        uint32_t w=width,h=height,format=0x34325241u,stride=w*4;
        if(is_case("invalid_fd")) fd=-1;
        if(is_case("wrong_size")) --w;
        if(is_case("wrong_stride")) --stride;
        if(is_case("wrong_format")) format=0;
        errno=0;
        const int result=commit(fd,w,h,format,stride,UINT64_MAX);
        assert(expect_failure?result==-1:result==0);
        if(is_case("deadline")) assert(errno==ETIMEDOUT);
        if(is_case("wrong_sequence")) assert(errno==EPROTO);
        if(is_case("startup_wrong_bytes")) assert(!display_queries && !attempts && !created);
        assert(created==destroyed && held_fence<0);
        if(duplicate_fd>=0) assert(fcntl(duplicate_fd,F_GETFD)<0 && errno==EBADF);
    }
    if(expect_failure) {
        const unsigned calls=attempts;
        assert(commit(buffer,width,height,0x34325241u,width*4,0)==-1 && attempts==calls);
        assert(mode(&width,&height)==-1);
        assert(shutdown_requests==1);
    } else {
        assert(queued==frames && created==frames && flushed==frames);
        assert(!shutdown_requests);
    }
    assert(!close(buffer) && fd_count()==before);
    printf("SESSION_CPU_CONTROL frames=%u attempts=%u fdBalanced=true argvPreserved=true gpuMock=true\n",queued,attempts);
    return is_case("main_failure")?37:0;
}
