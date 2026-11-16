/* SPDX-License-Identifier: MIT */
/* Owned guest-kernel handle/lifetime controls. No GPU is exercised here. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "wddm_aperture.h"
struct prime_handle { uint32_t handle, flags; int32_t fd; };
struct gem_close { uint32_t handle, pad; };
struct get_cap { uint64_t capability, value; };
#define PRIME_IMPORT _IOWR('d', 0x2e, struct prime_handle)
#define GEM_CLOSE _IOW('d', 0x09, struct gem_close)
#define GET_CAP _IOWR('d', 0x0c, struct get_cap)
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr,"NATIVE_PRIME_CONTRACT_FAIL line=%u errno=%d\n",__LINE__,errno); exit(1); } } while(0)
static unsigned import(int device,int fd) { struct prime_handle p={.fd=fd}; CHECK(!ioctl(device,PRIME_IMPORT,&p) && p.handle);return p.handle; }
int main(int argc,char** argv)
{
    CHECK(argc==2);
    const int control=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC), device=open(argv[1],O_RDWR|O_CLOEXEC);
    CHECK(control>=0 && device>=0);
    struct get_cap cap={.capability=5}; CHECK(!ioctl(device,GET_CAP,&cap) && cap.value==1);
    struct wddm_object_create object={.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=1234};
    CHECK(!ioctl(control,WDDM_OBJECT_CREATE,&object) && object.fd>=0 && object.cookie);
    struct wddm_native_buffer_create buffer={.version=1,.native_fd=object.fd,.bytes=4096};
    CHECK(!ioctl(control,WDDM_NATIVE_BUFFER_CREATE,&buffer) && buffer.fd>=0);
    const unsigned handle=import(device,buffer.fd);
    for(unsigned n=0;n<96;++n) CHECK(import(device,buffer.fd)==handle);
    struct prime_handle foreign={.fd=control}; CHECK(ioctl(device,PRIME_IMPORT,&foreign)==-1);
    foreign.fd=-1; CHECK(ioctl(device,PRIME_IMPORT,&foreign)==-1 && errno==EBADF);
    foreign=(struct prime_handle){.fd=buffer.fd,.flags=1}; CHECK(ioctl(device,PRIME_IMPORT,&foreign)==-1 && errno==EINVAL);
    CHECK(mmap(NULL,4096,PROT_READ,MAP_SHARED,device,0)==MAP_FAILED);
    struct gem_close closing={.handle=handle}; CHECK(!ioctl(device,GEM_CLOSE,&closing));
    CHECK(ioctl(device,GEM_CLOSE,&closing)==-1 && errno==EINVAL);
    const pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!close(control) && !close(device));
        CHECK(!setgroups(0,NULL) && !setgid(1000) && !setuid(1000));
        const int ordinary=open(argv[1],O_RDWR|O_CLOEXEC);CHECK(ordinary>=0);
        (void)import(ordinary,buffer.fd);CHECK(!close(ordinary));_exit(0);
    }
    int status=0; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    int files[256];
    for(unsigned n=0;n<256;++n) { files[n]=open(argv[1],O_RDWR|O_CLOEXEC);CHECK(files[n]>=0);(void)import(files[n],buffer.fd); }
    foreign=(struct prime_handle){.fd=buffer.fd};
    CHECK(ioctl(device,PRIME_IMPORT,&foreign)==-1 && errno==ENOSPC);
    CHECK(!close(buffer.fd) && !close(object.fd));
    struct wddm_native_buffer_stats stats={.version=1};
    CHECK(!ioctl(control,WDDM_NATIVE_BUFFER_STATS,&stats) && stats.held==1);
    for(unsigned n=0;n<256;++n) CHECK(!close(files[n]));
    stats=(struct wddm_native_buffer_stats){.version=1};
    CHECK(!ioctl(control,WDDM_NATIVE_BUFFER_STATS,&stats) && stats.held==0 && stats.created==stats.released);
    struct wddm_object_reap reap={.version=1};
    CHECK(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.ticket==1234 && reap.cookie==object.cookie && reap.kind==WDDM_OBJECT_RESOURCE);
    CHECK(!close(device) && !close(control));
    printf("NATIVE_PRIME_CONTRACT_PASS checks=%u repeatedImportSameHandle=true globalLimit=256 retainedAfterFdClose=true fileExitReleases=true ordinaryUid=1000 foreignRejected=true cpuMap=false gpu=false\n",checks);
    return 0;
}
