/* SPDX-License-Identifier: MIT */
/* Included by the owned-guest acceptance probe. Uses separate check counts
 * so previously retained aperture evidence continues to mean 581 checks. */
static unsigned object_checks;
#define OBJECT_REQUIRE(test) do { if (!(test)) { \
  fprintf(stderr,"OBJECT_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); \
} ++object_checks; } while (0)
static struct wddm_object_create object_arg(unsigned ticket)
{
    return (struct wddm_object_create){.version=1,.kind=WDDM_OBJECT_SYNC,.ticket=ticket};
}
static struct wddm_object_stats object_stats(int control)
{
    struct wddm_object_stats stats={.version=1};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_STATS,&stats));
    return stats;
}
static void object_empty_reap(int control)
{
    struct wddm_object_reap reap={.version=1};
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_REAP,&reap)==-1 && errno==EAGAIN);
}
static void object_contract(int control)
{
    _Static_assert(sizeof(struct wddm_object_create)==40 && sizeof(struct wddm_object_resolve)==32 &&
        sizeof(struct wddm_object_reap)==24 && sizeof(struct wddm_object_stats)==40,"object FD ABI");
    struct wddm_object_stats stats=object_stats(control);
    OBJECT_REQUIRE(!stats.held && !stats.active);
    object_empty_reap(control);
    for (unsigned n=0;n<9;++n) {
        struct wddm_object_create bad=object_arg(500);
        switch(n) {
            case 0: bad.version=2; break;
            case 1: bad.kind=3; break;
            case 2: bad.ticket=0; break;
            case 3: bad.ticket=UINT32_MAX; break;
            case 4: bad.cookie=1; break;
            case 5: bad.fd=1; break;
            case 6: bad.reserved[0]=1; break;
            case 7: bad.reserved[1]=1; break;
            default: bad.fd=-1; break;
        }
        errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,&bad)==-1 && errno==EINVAL);
    }
    struct wddm_object_create object=object_arg(500);
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&object));
    OBJECT_REQUIRE(object.fd>=0 && object.cookie && (fcntl(object.fd,F_GETFD)&FD_CLOEXEC));
    struct wddm_object_create duplicate=object_arg(500);
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,&duplicate)==-1 && errno==EEXIST);
    struct wddm_object_resolve resolve={.version=1,.kind=1,.fd=object.fd};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_RESOLVE,&resolve) && resolve.ticket==500 && resolve.cookie==object.cookie);
    errno=0; OBJECT_REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,object.fd,0)==MAP_FAILED && errno==ENODEV);
    int other=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); OBJECT_REQUIRE(other>=0);
    resolve=(struct wddm_object_resolve){.version=1,.kind=1,.fd=object.fd};
    errno=0; OBJECT_REQUIRE(ioctl(other,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==EPERM);
    struct wddm_aperture_revoke revoke={.version=1,.cookie=object.cookie};
    errno=0; OBJECT_REQUIRE(ioctl(other,WDDM_OBJECT_REVOKE,&revoke)==-1 && errno==ENOENT);
    close(other);
    int forged=(int)syscall(SYS_memfd_create,"forged-wddm-object",MFD_CLOEXEC); OBJECT_REQUIRE(forged>=0);
    resolve=(struct wddm_object_resolve){.version=1,.kind=1,.fd=forged};
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==EINVAL);
    close(forged);
    int copy=dup(object.fd); OBJECT_REQUIRE(copy>=0); close(object.fd);
    object_empty_reap(control); close(copy);
    struct wddm_object_reap reap={.version=1};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.ticket==500 && reap.cookie==object.cookie);
    object_empty_reap(control);
    object=object_arg(501); OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&object));
    revoke=(struct wddm_aperture_revoke){.version=1,.cookie=object.cookie};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_REVOKE,&revoke));
    resolve=(struct wddm_object_resolve){.version=1,.kind=1,.fd=object.fd};
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==ENODEV);
    reap=(struct wddm_object_reap){.version=1};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.ticket==501);
    close(object.fd);
    void* ro=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); OBJECT_REQUIRE(ro!=MAP_FAILED);
    *(struct wddm_object_create*)ro=object_arg(502);
    OBJECT_REQUIRE(!mprotect(ro,4096,PROT_READ));
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,ro)==-1 && errno==EFAULT);
    stats=object_stats(control); OBJECT_REQUIRE(!stats.held && !stats.active);
    OBJECT_REQUIRE(!mprotect(ro,4096,PROT_READ|PROT_WRITE));
    object=object_arg(502); OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&object)); close(object.fd);
    *(struct wddm_object_reap*)ro=(struct wddm_object_reap){.version=1};
    OBJECT_REQUIRE(!mprotect(ro,4096,PROT_READ));
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_REAP,ro)==-1 && errno==EFAULT);
    reap=(struct wddm_object_reap){.version=1};
    OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.ticket==502 && reap.cookie==object.cookie);
    munmap(ro,4096);
    int fds[WDDM_OBJECT_MAX];
    for (unsigned n=0;n<WDDM_OBJECT_MAX;++n) {
        object=object_arg(1000+n); OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&object)); fds[n]=object.fd;
    }
    object=object_arg(2000); errno=0;
    OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,&object)==-1 && errno==ENOSPC);
    for (unsigned n=0;n<WDDM_OBJECT_MAX;++n) close(fds[n]);
    stats=object_stats(control); OBJECT_REQUIRE(stats.held==WDDM_OBJECT_MAX && !stats.active);
    for (unsigned n=0;n<WDDM_OBJECT_MAX;++n) {
        reap=(struct wddm_object_reap){.version=1};
        OBJECT_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.ticket==1000+n);
    }
    other=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); OBJECT_REQUIRE(other>=0);
    object=object_arg(3000); OBJECT_REQUIRE(!ioctl(other,WDDM_OBJECT_CREATE,&object)); close(other);
    resolve=(struct wddm_object_resolve){.version=1,.kind=1,.fd=object.fd};
    errno=0; OBJECT_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==ENODEV);
    close(object.fd);
    stats=object_stats(control); OBJECT_REQUIRE(!stats.held && !stats.active && stats.created==stats.closed);
    object_empty_reap(control);
    printf("OBJECT_CONTRACT_PASS checks=%u held=0 active=0 forgedRejected=true ownerIsolation=true dupLifetime=true reapRollback=true\n",object_checks);
}
#include "resource_object_contract.h"
