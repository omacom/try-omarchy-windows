// SPDX-License-Identifier: MIT
// Explicit guest-only module preparation. Never invoked by CPU/build controls.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "guest_session_devices.h"
static int load_root_module(const char* path,const char* parameters) {
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return -1;
    struct stat state;
    if(fstat(fd,&state) || !S_ISREG(state.st_mode) || state.st_uid || (state.st_mode&0022) || state.st_nlink!=1) { close(fd);return -1; }
    const int result=(int)syscall(SYS_finit_module,fd,parameters,0);const int error=errno;
    if(close(fd)) return -1;
    return !result || error==EEXIST?0:-1;
}
static int enabled(const char* path) {
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -1;
    char value[4]={0};struct stat state;
    const int valid=!fstat(fd,&state) && !state.st_uid && !(state.st_mode&0022) && read(fd,value,sizeof value)==2 && !memcmp(value,"Y\n",2);
    const int closed=close(fd);return valid && !closed?0:-1;
}
int main(void) {
    if(getpid()==1 || getuid() || geteuid() || getgid() || getegid()) return 125;
    char port[128],render[64];
    // Require the named guest bridge before any module syscall or file change.
    if(wddm_graphics_port_resolve("/sys/class/virtio-ports","/dev",port,sizeof port)) return 125;
    const char* parameters="enable_native_sync_files=1 enable_native_buffers=1 enable_desktop_4k_cpu=1 "
        "enable_sparse_aperture=1 enable_coordinated_leases=1 enable_desktop_fences=1 "
        "enable_desktop_slots=1 enable_desktop_leases=1 enable_multiapp_cpu=1";
    if(load_root_module("/opt/omarchy-nvgpu/kernel/wddm-aperture.ko",parameters) ||
       load_root_module("/opt/omarchy-nvgpu/kernel/wddm-render.ko","experimental=1 enable_native_prime=1") ||
       enabled("/sys/module/wddm_aperture/parameters/enable_native_sync_files") ||
       enabled("/sys/module/wddm_aperture/parameters/enable_native_buffers") ||
       enabled("/sys/module/wddm_render/parameters/experimental") ||
       enabled("/sys/module/wddm_render/parameters/enable_native_prime") ||
       wddm_session_devices_ready(port,render)) return 125;
    return 0;
}
