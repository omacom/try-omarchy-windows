/* SPDX-License-Identifier: MIT */
/* Structural/rollback checks only. Never queue a read from an unbacked page. */
static void native_sync_lineage_contract(int control)
{
    unsigned count=0;
#define LINEAGE_REQUIRE(x) do { if (!(x)) { fprintf(stderr,"SYNC_LINEAGE_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } ++count; } while (0)
    struct wddm_sync_file_native_create value={.sync={.version=1,.cookie=1,.value=1},.native_fd=-1};
    struct wddm_sync_file_native_resolve resolve={.version=1,.fd=-1};
    _Static_assert(sizeof(value)==48 && sizeof(resolve)==32,"Fixed sync-file lineage ABI");
    errno=0; int result=ioctl(control,WDDM_SYNC_FILE_NATIVE_CREATE,&value);
    if(result==-1 && errno==EOPNOTSUPP) {
        errno=0; LINEAGE_REQUIRE(ioctl(control,WDDM_SYNC_FILE_NATIVE_RESOLVE,&resolve)==-1 && errno==EOPNOTSUPP);
        puts("SYNC_LINEAGE_CONTRACT disabled=true gpu=false"); return;
    }
    LINEAGE_REQUIRE(result==-1 && errno==EINVAL);
    int owner=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC);
    LINEAGE_REQUIRE(owner>=0);
    struct wddm_aperture_create lease=fence();
    LINEAGE_REQUIRE(!ioctl(owner,WDDM_APERTURE_CREATE,&lease));
    struct wddm_object_create native={.version=1,.kind=WDDM_OBJECT_SYNC,.ticket=8000};
    struct wddm_object_create resource={.version=1,.kind=WDDM_OBJECT_RESOURCE,.ticket=8001};
    LINEAGE_REQUIRE(!ioctl(owner,WDDM_OBJECT_CREATE,&native));
    LINEAGE_REQUIRE(!ioctl(owner,WDDM_OBJECT_CREATE,&resource));
    int foreign=open("/dev/null",O_RDONLY|O_CLOEXEC); LINEAGE_REQUIRE(foreign>=0);
    for(unsigned n=0;n<10;++n) {
        value=(struct wddm_sync_file_native_create){.sync={.version=1,.cookie=lease.cookie,.value=1},.native_fd=native.fd};
        if(n==0) value.reserved=1;
        if(n==1) value.native_fd=-1;
        if(n==2) value.native_fd=foreign;
        if(n==3) value.native_fd=resource.fd;
        if(n==4) value.sync.flags=1;
        if(n==5) value.sync.offset=1;
        if(n==6) value.sync.offset=4096;
        if(n==7) value.sync.value=UINT64_MAX;
        if(n==8) value.sync.fd=1;
        if(n==9) value.sync.reserved=1;
        errno=0; LINEAGE_REQUIRE(ioctl(owner,WDDM_SYNC_FILE_NATIVE_CREATE,&value)==-1 && errno==EINVAL);
    }
    value=(struct wddm_sync_file_native_create){.sync={.version=1,.cookie=lease.cookie,.value=1},.native_fd=native.fd};
    errno=0; LINEAGE_REQUIRE(ioctl(control,WDDM_SYNC_FILE_NATIVE_CREATE,&value)==-1 && errno==ENOENT);
    for(unsigned n=0;n<7;++n) {
        resolve=(struct wddm_sync_file_native_resolve){.version=1,.fd=foreign};
        if(n==0) resolve.version=2;
        if(n==1) resolve.flags=1;
        if(n==2) resolve.fd=-1;
        if(n==3) resolve.native_fd=1;
        if(n==4) resolve.value=1;
        if(n==5) resolve.reserved=1;
        if(n==6) resolve.fd=native.fd;
        errno=0; LINEAGE_REQUIRE(ioctl(owner,WDDM_SYNC_FILE_NATIVE_RESOLVE,&resolve)==-1 && errno==EINVAL);
    }
    /* Linked creation keeps a second native object file reference. Copyout
     * failure must release both retained source files before any poll starts. */
    void* readonly=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    LINEAGE_REQUIRE(readonly!=MAP_FAILED);
    *(struct wddm_sync_file_native_create*)readonly=value;
    LINEAGE_REQUIRE(!mprotect(readonly,4096,PROT_READ));
    errno=0; LINEAGE_REQUIRE(ioctl(owner,WDDM_SYNC_FILE_NATIVE_CREATE,readonly)==-1 && errno==EFAULT);
    LINEAGE_REQUIRE(!munmap(readonly,4096));
    struct wddm_aperture_revoke revoke={.version=1,.cookie=native.cookie};
    LINEAGE_REQUIRE(!ioctl(owner,WDDM_OBJECT_REVOKE,&revoke));
    errno=0; LINEAGE_REQUIRE(ioctl(owner,WDDM_SYNC_FILE_NATIVE_CREATE,&value)==-1 && errno==ENODEV);
    close(lease.fd); close(native.fd); close(resource.fd); close(foreign); close(owner);
    struct wddm_sync_file_stats stats;
    for(unsigned n=0;n<2000;++n) {
        stats=(struct wddm_sync_file_stats){.version=1};
        LINEAGE_REQUIRE(!ioctl(control,WDDM_SYNC_FILE_STATS,&stats));
        if(!stats.held && !stats.pending) break;
        usleep(1000);
    }
    LINEAGE_REQUIRE(!stats.held && !stats.pending && !stats.created && !stats.signaled && !stats.errors && !stats.released);
    printf("SYNC_LINEAGE_CONTRACT_PASS checks=%u enabled=true rollback=true leaseOwnerIsolation=true foreignRejected=true gpu=false\n",count);
#undef LINEAGE_REQUIRE
}
