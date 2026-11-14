// SPDX-License-Identifier: MIT
// Verify the already prepared guest-only profile; no GPU admission is broadened.
#pragma once
#include "guest_graphics_port.h"
#include "guest-kernel/wddm_aperture.h"
#include "guest-kernel/wddm_render.h"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
static int wddm_session_aperture_ready(void) {
    const int fd=open("/dev/wddm-aperture",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return -1;
    struct stat state;
    struct wddm_aperture_info info={.version=WDDM_APERTURE_VERSION};
    struct wddm_aperture_caps caps={.version=1};
    struct wddm_aperture_coordinator_caps coordinated={.version=1};
    struct wddm_aperture_desktop_fences fences={.version=1};
    struct wddm_aperture_desktop_slots slots={.version=1};
    struct wddm_object_stats objects={.version=1};
    const int valid=!fstat(fd,&state) && S_ISCHR(state.st_mode) && !state.st_uid &&
        !ioctl(fd,WDDM_APERTURE_INFO,&info) && info.version==1 && info.fence_slots==512 && info.allocation_slots==1024 &&
        !info.active_leases && !info.active_cpu_bytes &&
        !ioctl(fd,WDDM_APERTURE_CAPS,&caps) && wddm_aperture_valid_caps(caps) && caps.flags==7 &&
        !ioctl(fd,WDDM_APERTURE_COORDINATOR_CAPS,&coordinated) && wddm_aperture_valid_coordinator_caps(coordinated) &&
        coordinated.enabled && coordinated.lease_limit==WDDM_APERTURE_DESKTOP_LEASES &&
        !ioctl(fd,WDDM_APERTURE_DESKTOP_FENCES,&fences) && wddm_aperture_valid_desktop_fences(fences) && fences.enabled &&
        !ioctl(fd,WDDM_APERTURE_DESKTOP_SLOTS,&slots) && wddm_aperture_valid_desktop_slots(slots) && slots.enabled &&
        !ioctl(fd,WDDM_OBJECT_STATS,&objects) && !objects.held && !objects.active;
    const int closed=close(fd);
    if(!valid || closed) { errno=EPROTO;return -1; }
    return 0;
}
static int wddm_session_devices_ready(char port[128],char render[64]) {
    if(wddm_graphics_port_resolve("/sys/class/virtio-ports","/dev",port,128) || wddm_session_aperture_ready()) return -1;
    unsigned matches=0;
    for(unsigned index=128;index<256;++index) {
        char path[64];snprintf(path,sizeof path,"/dev/dri/renderD%u",index);
        const int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0) { if(errno==ENOENT || errno==EACCES) continue;return -1; }
        struct wddm_render_info info={.version=WDDM_RENDER_VERSION};struct stat state;
        const int valid=!fstat(fd,&state) && S_ISCHR(state.st_mode) && !state.st_uid &&
            !ioctl(fd,DRM_IOCTL_WDDM_RENDER_INFO,&info) && info.version==1 && info.flags==WDDM_RENDER_NATIVE_ROUTE &&
            info.signature==WDDM_RENDER_MAGIC && info.max_width==4096 && info.max_height==4096 && !info.reserved[0] && !info.reserved[1];
        if(close(fd)) return -1;
        if(valid) { ++matches;memcpy(render,path,strlen(path)+1); }
    }
    if(matches!=1) { errno=ENODEV;return -1; }
    struct stat native;
    if(lstat(port,&native) || !S_ISCHR(native.st_mode) || native.st_uid || chmod(port,0600) || chmod(render,0666)) return -1;
    return 0;
}
