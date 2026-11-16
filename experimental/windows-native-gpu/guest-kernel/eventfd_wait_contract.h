/* SPDX-License-Identifier: MIT */
/* Invalid requests must never publish a watch or read an unbacked PCI page. */
#include <sys/eventfd.h>
static void eventfd_wait_contract(int control)
{
    struct wddm_fence_eventfd_stats stats={.version=1};
    unsigned count=0;
    int result=ioctl(control,WDDM_FENCE_EVENTFD_STATS,&stats);
#define EVENT_REQUIRE(x) do { if (!(x)) { fprintf(stderr,"EVENTFD_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } ++count; } while (0)
    if(result==-1 && (errno==ENOTTY || errno==EOPNOTSUPP)) {
        puts("EVENTFD_CONTRACT supported=false gpu=false"); return;
    }
    EVENT_REQUIRE(!result && !stats.created && !stats.held && !stats.pending);
    _Static_assert(sizeof(struct wddm_fence_eventfd_point)==24 &&
        sizeof(struct wddm_fence_eventfd_wait)==40 && sizeof(stats)==48,"Fixed eventfd ABI");
    const uint64_t mmio=info(control).fence_mmio_reads;
    struct wddm_aperture_create source=fence();
    EVENT_REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&source));
    const int event=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    EVENT_REQUIRE(event>=0);
    struct wddm_fence_eventfd_point points[2]={{.lease_fd=source.fd,.value=1},{.lease_fd=-1,.value=1}};
    struct wddm_fence_eventfd_wait base={.version=1,.count=1,.event_fd=event,.points=(uintptr_t)points};
    for(unsigned n=0;n<9;++n) {
        struct wddm_fence_eventfd_wait bad=base;
        switch(n) {
        case 0: bad.version=2; break;
        case 1: bad.flags=2; break;
        case 2: bad.count=0; break;
        case 3: bad.count=WDDM_FENCE_EVENTFD_MAX_POINTS+1; break;
        case 4: bad.event_fd=-1; break;
        case 5: bad.points=0; break;
        case 6: bad.reserved[0]=1; break;
        case 7: bad.reserved[1]=1; break;
        case 8: bad.points=1; break;
        }
        errno=0;
        EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&bad)==-1 && errno==(n==8 ? EFAULT : EINVAL));
    }
    for(unsigned n=0;n<8;++n) {
        points[0]=(struct wddm_fence_eventfd_point){.lease_fd=source.fd,.value=1};
        struct wddm_fence_eventfd_wait bad=base;
        switch(n) {
        case 0: points[0].lease_fd=-1; break;
        case 1: points[0].lease_fd=0x3fffffff; break;
        case 2: points[0].reserved=1; break;
        case 3: points[0].offset=1; break;
        case 4: points[0].offset=4096; break;
        case 5: points[0].value=UINT64_MAX; break;
        case 6: points[0].lease_fd=event; break;
        case 7: bad.event_fd=source.fd; break;
        }
        errno=0;
        EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&bad)==-1 && errno==(n==1 ? EBADF : EINVAL));
    }
    points[0]=(struct wddm_fence_eventfd_point){.lease_fd=source.fd,.value=1};
    /* First point preparation must roll back when a later point is invalid. */
    base.count=2; errno=0;
    EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&base)==-1 && errno==EINVAL);
    base.count=1;
    const int foreign=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC);
    EVENT_REQUIRE(foreign>=0);
    struct wddm_aperture_create other=fence(); other.offset=4096;
    EVENT_REQUIRE(!ioctl(foreign,WDDM_APERTURE_CREATE,&other));
    points[0].lease_fd=other.fd; errno=0;
    EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&base)==-1 && errno==EPERM);
    revoke_lease(foreign,other.cookie); close(other.fd); close(foreign);
    struct wddm_aperture_create allocation={.version=1,.kind=WDDM_APERTURE_ALLOCATION,
        .bytes=4096,.host_generation=1,.flags=WDDM_APERTURE_WRITE};
    EVENT_REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&allocation));
    points[0].lease_fd=allocation.fd; errno=0;
    EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&base)==-1 && errno==EINVAL);
    errno=0;
    EVENT_REQUIRE(ioctl(allocation.fd,WDDM_FENCE_EVENTFD_WAIT,&base)==-1 && errno==EINVAL);
    revoke_lease(control,allocation.cookie); close(allocation.fd);
    points[0].lease_fd=source.fd;
    revoke_lease(control,source.cookie); errno=0;
    EVENT_REQUIRE(ioctl(source.fd,WDDM_FENCE_EVENTFD_WAIT,&base)==-1 && errno==ENODEV);
    close(source.fd); close(event);
    stats=(struct wddm_fence_eventfd_stats){.version=1};
    EVENT_REQUIRE(!ioctl(control,WDDM_FENCE_EVENTFD_STATS,&stats));
    EVENT_REQUIRE(!stats.created && !stats.notified && !stats.errors && !stats.held && !stats.pending && !stats.released);
    EVENT_REQUIRE(info(control).fence_mmio_reads==mmio);
    printf("EVENTFD_CONTRACT_PASS checks=%u published=0 notified=0 gpu=false partialRollback=true foreignOwnerRejected=true\n",count);
#undef EVENT_REQUIRE
}
