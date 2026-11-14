/* SPDX-License-Identifier: MIT */
/* Resource and fence FDs share lifetime rules, but cannot substitute types. */
static void resource_object_contract(int control)
{
    unsigned checks=0;
#define RESOURCE_REQUIRE(test) do { if (!(test)) { \
    fprintf(stderr,"RESOURCE_FD_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); \
} ++checks; } while (0)
    struct wddm_object_create object={.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=4000};
    RESOURCE_REQUIRE(!ioctl(control,WDDM_OBJECT_CREATE,&object));
    RESOURCE_REQUIRE(object.fd>=0 && object.cookie && (fcntl(object.fd,F_GETFD)&FD_CLOEXEC));
    struct wddm_object_resolve resolve={.version=1,.kind=WDDM_OBJECT_RESOURCE,.fd=object.fd};
    RESOURCE_REQUIRE(!ioctl(control,WDDM_OBJECT_RESOLVE,&resolve) && resolve.ticket==4000 && resolve.cookie==object.cookie);
    resolve=(struct wddm_object_resolve){.version=1,.kind=WDDM_OBJECT_SYNC,.fd=object.fd};
    errno=0; RESOURCE_REQUIRE(ioctl(control,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==EPERM);
    struct wddm_object_create duplicate={.version=1,.kind=WDDM_OBJECT_SYNC,.ticket=4000};
    errno=0; RESOURCE_REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,&duplicate)==-1 && errno==EEXIST);
    int other=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); RESOURCE_REQUIRE(other>=0);
    resolve=(struct wddm_object_resolve){.version=1,.kind=WDDM_OBJECT_RESOURCE,.fd=object.fd};
    errno=0; RESOURCE_REQUIRE(ioctl(other,WDDM_OBJECT_RESOLVE,&resolve)==-1 && errno==EPERM);
    RESOURCE_REQUIRE(!close(other));
    errno=0; RESOURCE_REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,object.fd,0)==MAP_FAILED && errno==ENODEV);
    int copy=dup(object.fd); RESOURCE_REQUIRE(copy>=0);
    RESOURCE_REQUIRE(!close(object.fd));
    struct wddm_object_reap reap={.version=1};
    errno=0; RESOURCE_REQUIRE(ioctl(control,WDDM_OBJECT_REAP,&reap)==-1 && errno==EAGAIN);
    RESOURCE_REQUIRE(!close(copy));
    reap=(struct wddm_object_reap){.version=1};
    RESOURCE_REQUIRE(!ioctl(control,WDDM_OBJECT_REAP,&reap) && reap.kind==WDDM_OBJECT_RESOURCE && reap.ticket==4000 && reap.cookie==object.cookie);
    struct wddm_object_stats stats={.version=1};
    RESOURCE_REQUIRE(!ioctl(control,WDDM_OBJECT_STATS,&stats) && !stats.held && !stats.active && stats.created==stats.closed);
    printf("RESOURCE_FD_CONTRACT_PASS checks=%u held=0 active=0 typeIsolation=true ownerIsolation=true dupLifetime=true dmaBuf=false\n",checks);
#undef RESOURCE_REQUIRE
}
