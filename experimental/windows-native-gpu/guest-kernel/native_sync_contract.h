/* SPDX-License-Identifier: MIT */
/* Reject invalid exports without reading an unbacked synthetic PCI page. */
static void native_sync_contract(int control)
{
    struct wddm_sync_file_create value={.version=1,.cookie=1,.value=1};
    struct wddm_sync_file_stats stats={.version=1};
    unsigned count=0;
#define SYNC_REQUIRE(x) do { if (!(x)) { fprintf(stderr,"SYNC_FILE_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } ++count; } while (0)
    _Static_assert(sizeof(value)==40 && sizeof(stats)==48,"Fixed sync-file ABI");
    errno=0; int result=ioctl(control,WDDM_SYNC_FILE_CREATE,&value);
    if(result==-1 && errno==EOPNOTSUPP) { puts("SYNC_FILE_CONTRACT disabled=true gpu=false"); return; }
    SYNC_REQUIRE(result==-1 && errno==ENOENT);
    struct wddm_aperture_create source=fence();
    SYNC_REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&source));
    for(unsigned n=0;n<7;++n) {
        value=(struct wddm_sync_file_create){.version=1,.cookie=source.cookie,.value=1};
        if(n==0) value.version=2;
        if(n==1) value.flags=1;
        if(n==2) value.value=0;
        if(n==3) value.value=UINT64_MAX;
        if(n==4) value.offset=1;
        if(n==5) value.fd=1;
        if(n==6) value.reserved=1;
        errno=0; SYNC_REQUIRE(ioctl(control,WDDM_SYNC_FILE_CREATE,&value)==-1 && errno==EINVAL);
    }
    value=(struct wddm_sync_file_create){.version=1,.cookie=source.cookie,.offset=4096,.value=1};
    errno=0; SYNC_REQUIRE(ioctl(control,WDDM_SYNC_FILE_CREATE,&value)==-1 && errno==EINVAL);
    const int foreign=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); SYNC_REQUIRE(foreign>=0);
    value.offset=0; errno=0; SYNC_REQUIRE(ioctl(foreign,WDDM_SYNC_FILE_CREATE,&value)==-1 && errno==ENOENT); close(foreign);
    void* readonly=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    SYNC_REQUIRE(readonly!=MAP_FAILED); *(struct wddm_sync_file_create*)readonly=value;
    SYNC_REQUIRE(!mprotect(readonly,4096,PROT_READ)); errno=0;
    /* Rollback occurs before a polling worker is queued. */
    SYNC_REQUIRE(ioctl(control,WDDM_SYNC_FILE_CREATE,readonly)==-1 && errno==EFAULT);
    munmap(readonly,4096); revoke_lease(control,source.cookie); close(source.fd);
    errno=0; SYNC_REQUIRE(ioctl(control,WDDM_SYNC_FILE_CREATE,&value)==-1 && errno==ENOENT);
    struct wddm_aperture_create cpu={.version=1,.kind=2,.bytes=4096,.host_generation=1,.flags=1};
    SYNC_REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&cpu)); value.cookie=cpu.cookie; errno=0;
    SYNC_REQUIRE(ioctl(control,WDDM_SYNC_FILE_CREATE,&value)==-1 && errno==EINVAL);
    revoke_lease(control,cpu.cookie); close(cpu.fd);
    for(unsigned n=0;n<2000;++n) {
        stats=(struct wddm_sync_file_stats){.version=1}; SYNC_REQUIRE(!ioctl(control,WDDM_SYNC_FILE_STATS,&stats));
        if(!stats.held && !stats.pending) break;
        usleep(1000);
    }
    SYNC_REQUIRE(!stats.held && !stats.pending && !stats.created && !stats.signaled && !stats.errors && !stats.released);
    printf("SYNC_FILE_CONTRACT_PASS checks=%u enabled=true rollback=true ownerIsolation=true gpu=false\n",count);
#undef SYNC_REQUIRE
}
