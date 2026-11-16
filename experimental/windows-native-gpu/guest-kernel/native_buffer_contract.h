/* SPDX-License-Identifier: MIT */
#include <linux/dma-buf.h>
static int native_buffer_caps_contract(int control)
{
    unsigned checks=0;
#define CAPS_REQUIRE(test) do { if(!(test)) { fprintf(stderr,"NATIVE_BUFFER_CAPS_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } ++checks; } while(0)
    _Static_assert(sizeof(struct wddm_native_buffer_caps)==16,"native buffer caps ABI");
    struct wddm_native_buffer_caps caps={.version=1};
    CAPS_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_CAPS,&caps) && caps.version==1 &&
        !(caps.flags&~WDDM_NATIVE_BUFFER_ENABLED) && !caps.reserved[0] && !caps.reserved[1]);
    const int enabled=(caps.flags&WDDM_NATIVE_BUFFER_ENABLED)!=0;
    for(unsigned n=0;n<4;++n) {
        caps=(struct wddm_native_buffer_caps){.version=1};
        if(n==0) caps.version=2;
        if(n==1) caps.flags=1;
        if(n==2) caps.reserved[0]=1;
        if(n==3) caps.reserved[1]=1;
        errno=0; CAPS_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CAPS,&caps)==-1 && errno==EINVAL);
    }
    void* readonly=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CAPS_REQUIRE(readonly!=MAP_FAILED);
    *(struct wddm_native_buffer_caps*)readonly=(struct wddm_native_buffer_caps){.version=1};
    CAPS_REQUIRE(!mprotect(readonly,4096,PROT_READ));
    errno=0; CAPS_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CAPS,readonly)==-1 && errno==EFAULT);
    CAPS_REQUIRE(!munmap(readonly,4096));
    printf("NATIVE_BUFFER_CAPS_CONTRACT_PASS checks=%u enabled=%s readOnly=true gpu=false\n",checks,enabled?"true":"false");
#undef CAPS_REQUIRE
    return enabled;
}
static void native_buffer_contract(int control)
{
    const int caps_enabled=native_buffer_caps_contract(control);
    unsigned count=0;
#define BUFFER_REQUIRE(test) do { if (!(test)) { \
    fprintf(stderr,"NATIVE_BUFFER_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); \
} ++count; } while (0)
    _Static_assert(sizeof(struct wddm_native_buffer_create)==32,"native buffer create ABI");
    _Static_assert(sizeof(struct wddm_native_buffer_info)==24,"native buffer info ABI");
    _Static_assert(sizeof(struct wddm_native_buffer_stats)==32,"native buffer stats ABI");
    struct wddm_native_buffer_create create={.version=1,.native_fd=-1,.bytes=4096};
    errno=0; int result=ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create);
    if (result==-1 && errno==EOPNOTSUPP) {
        if(caps_enabled) { fputs("NATIVE_BUFFER_CAPS_CONTRACT_FAIL inconsistent=true\n",stderr); exit(1); }
        puts("NATIVE_BUFFER_CONTRACT disabled=true gpu=false"); return;
    }
    if(!caps_enabled) { fputs("NATIVE_BUFFER_CAPS_CONTRACT_FAIL inconsistent=true\n",stderr); exit(1); }
    BUFFER_REQUIRE(result==-1 && errno==EINVAL);
    struct wddm_object_create source={.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=5000};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&source));
    const struct wddm_native_buffer_create valid={.version=1,.native_fd=source.fd,.bytes=4096};
    create=valid; BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create));
    BUFFER_REQUIRE(create.fd>=0 && (fcntl(create.fd,F_GETFD)&FD_CLOEXEC));
    BUFFER_REQUIRE(lseek(create.fd,0,SEEK_END)==4096 && lseek(create.fd,0,SEEK_SET)==0);
    struct wddm_native_buffer_info info={.version=1,.fd=create.fd};
    BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_INFO,&info) && info.bytes==4096);
    struct wddm_object_resolve resolve={.version=1,.kind=WDDM_OBJECT_RESOURCE,.fd=create.fd};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_RESOLVE,&resolve) && resolve.ticket==5000 && resolve.cookie==source.cookie);
    resolve=(struct wddm_object_resolve){.version=1,.kind=WDDM_OBJECT_SYNC,.fd=create.fd};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==EPERM);
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
    errno=0; BUFFER_REQUIRE(ioctl(create.fd,DMA_BUF_IOCTL_SYNC,&sync)==-1 && errno==EOPNOTSUPP);
    errno=0; BUFFER_REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,create.fd,0)==MAP_FAILED && errno==ENODEV);
    int other=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); BUFFER_REQUIRE(other>=0);
    info=(struct wddm_native_buffer_info){.version=1,.fd=create.fd};
    errno=0; BUFFER_REQUIRE(ioctl(other,WDDM_NATIVE_BUFFER_INFO,&info)==-1 && errno==EPERM);
    struct wddm_native_buffer_create bad=valid;
    errno=0; BUFFER_REQUIRE(ioctl(other,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EPERM);
    BUFFER_REQUIRE(!close(other));
    bad=valid; bad.native_fd=create.fd;
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EINVAL);
    info=(struct wddm_native_buffer_info){.version=1,.fd=source.fd};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_INFO,&info)==-1 && errno==EINVAL);
    for (unsigned n=0;n<5;++n) {
        bad=valid;
        if(n==0) bad.flags=1;
        if(n==1) bad.version=2;
        if(n==2) bad.bytes=0;
        if(n==3) bad.bytes=WDDM_NATIVE_BUFFER_MAX_BYTES+1;
        if(n==4) bad.reserved=1;
        errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EINVAL);
    }
    void* readonly=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    BUFFER_REQUIRE(readonly!=MAP_FAILED); *(struct wddm_native_buffer_create*)readonly=valid;
    BUFFER_REQUIRE(!mprotect(readonly,4096,PROT_READ));
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,readonly)==-1 && errno==EFAULT);
    BUFFER_REQUIRE(!munmap(readonly,4096));
    struct wddm_native_buffer_stats stats={.version=1};
    BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_STATS,&stats) && stats.held==1 && stats.bytes==4096 && stats.created==1 && !stats.released);
    pid_t child=fork(); BUFFER_REQUIRE(child>=0);
    if(!child) {
        if(setgroups(0,NULL) || setgid(65534) || setuid(65534)) _exit(1);
        bad=valid; errno=0;
        if(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)!=-1 || errno!=EPERM) _exit(2);
        info=(struct wddm_native_buffer_info){.version=1,.fd=create.fd}; errno=0;
        if(ioctl(control,WDDM_NATIVE_BUFFER_INFO,&info)!=-1 || errno!=EPERM) _exit(3);
        if(lseek(create.fd,0,SEEK_END)!=4096) _exit(4);
        _exit(0);
    }
    int status=0;
    BUFFER_REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    BUFFER_REQUIRE(!close(source.fd));
    resolve=(struct wddm_object_resolve){.version=1,.kind=WDDM_OBJECT_RESOURCE,.fd=create.fd};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_RESOLVE,&resolve) && resolve.cookie==source.cookie);
    int copy=dup(create.fd); BUFFER_REQUIRE(copy>=0 && !close(create.fd));
    struct wddm_object_reap reap={.version=1};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_OBJECT_REAP,&reap)==-1 && errno==EAGAIN);
    BUFFER_REQUIRE(!close(copy));
    reap=(struct wddm_object_reap){.version=1};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.kind==WDDM_OBJECT_RESOURCE && reap.cookie==source.cookie);
    /* Object quota, then byte quota, independently of the source-object quota. */
    source=(struct wddm_object_create){.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=5001};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&source));
    int fds[WDDM_NATIVE_BUFFER_MAX];
    for(unsigned n=0;n<WDDM_NATIVE_BUFFER_MAX;++n) {
        create=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=4096};
        BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create)); fds[n]=create.fd;
    }
    create=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=4096};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create)==-1 && errno==ENOSPC);
    for(unsigned n=0;n<WDDM_NATIVE_BUFFER_MAX;++n) BUFFER_REQUIRE(!close(fds[n]));
    for(unsigned n=0;n<4;++n) {
        create=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=WDDM_NATIVE_BUFFER_MAX_BYTES};
        BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create)); fds[n]=create.fd;
    }
    create=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=1};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&create)==-1 && errno==ENOSPC);
    for(unsigned n=0;n<4;++n) BUFFER_REQUIRE(!close(fds[n]));
    BUFFER_REQUIRE(!close(source.fd));
    reap=(struct wddm_object_reap){.version=1}; BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap));
    /* Fence capabilities and ordinary files cannot become native buffers. */
    source=(struct wddm_object_create){.version=1,.kind=WDDM_OBJECT_SYNC,.ticket=5002};
    BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&source));
    bad=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=4096};
    errno=0; BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EPERM);
    BUFFER_REQUIRE(!close(source.fd));
    reap=(struct wddm_object_reap){.version=1}; BUFFER_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap));
    int foreign=open("/dev/null",O_RDONLY|O_CLOEXEC); BUFFER_REQUIRE(foreign>=0);
    bad.native_fd=foreign; errno=0;
    BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EINVAL);
    BUFFER_REQUIRE(!close(foreign));
    bad.native_fd=INT32_MAX; errno=0;
    BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==EBADF);
    /* Both explicit revocation and controller exit invalidate live wrappers. */
    for(unsigned explicit_revoke=0;explicit_revoke<2;++explicit_revoke) {
        other=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); BUFFER_REQUIRE(other>=0);
        source=(struct wddm_object_create){.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=5010+explicit_revoke};
        BUFFER_REQUIRE(!ioctl(other,WDDM_OBJECT_CREATE,&source));
        create=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=4096};
        BUFFER_REQUIRE(!ioctl(other,WDDM_NATIVE_BUFFER_CREATE,&create));
        if(explicit_revoke) {
            struct wddm_aperture_revoke revoke={.version=1,.cookie=source.cookie};
            BUFFER_REQUIRE(!ioctl(other,WDDM_OBJECT_REVOKE,&revoke));
            bad=(struct wddm_native_buffer_create){.version=1,.native_fd=source.fd,.bytes=4096}; errno=0;
            BUFFER_REQUIRE(ioctl(other,WDDM_NATIVE_BUFFER_CREATE,&bad)==-1 && errno==ENODEV);
        }
        BUFFER_REQUIRE(!close(other));
        info=(struct wddm_native_buffer_info){.version=1,.fd=create.fd}; errno=0;
        BUFFER_REQUIRE(ioctl(control,WDDM_NATIVE_BUFFER_INFO,&info)==-1 && errno==ENODEV);
        resolve=(struct wddm_object_resolve){.version=1,.kind=WDDM_OBJECT_RESOURCE,.fd=create.fd}; errno=0;
        BUFFER_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==ENODEV);
        BUFFER_REQUIRE(!close(source.fd) && !close(create.fd));
    }
    stats=(struct wddm_native_buffer_stats){.version=1};
    BUFFER_REQUIRE(!ioctl(control,WDDM_NATIVE_BUFFER_STATS,&stats) && !stats.held && !stats.bytes && stats.created==stats.released);
    printf("NATIVE_BUFFER_CONTRACT_PASS checks=%u dmaBuf=true opaque=true cpuAccess=false foreignDma=false gpu=false\n",count);
#undef BUFFER_REQUIRE
}
