/* SPDX-License-Identifier: MIT */
/* One explicitly named output port; never probe keyboard/tablet nodes. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int trace_identity(const char* name,const char* device,dev_t* result) {
    if(!name || !device || !result || strcmp(name,"org.7wdev.graphics.trace\n")) return -1;
    unsigned major_number=0,minor_number=0;int used=0;
    if(sscanf(device,"%u:%u%n",&major_number,&minor_number,&used)!=2 || used<=0 ||
       strcmp(device+used,"\n") || !major_number || major_number>4095 || minor_number>1048575) return -1;
    /* Sysfs decimal identity has one canonical spelling. */
    char canonical[48];
    const int bytes=snprintf(canonical,sizeof canonical,"%u:%u\n",major_number,minor_number);
    if(bytes<=0 || (size_t)bytes>=sizeof canonical || strcmp(device,canonical)) return -1;
    *result=makedev(major_number,minor_number);return 0;
}
static int trace_read_attribute(const char* path,char* data,size_t capacity) {
    if(!path || !data || capacity<2) return -1;
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -1;
    size_t used=0;int ok=1;
    while(used<capacity-1) {
        const ssize_t count=read(fd,data+used,capacity-1-used);
        if(count<0 && errno==EINTR) continue;
        if(count<0) { ok=0;break; }
        if(!count) break;
        used+=(size_t)count;
    }
    if(used==capacity-1) ok=0;
    data[used]=0;if(close(fd)) ok=0;
    return ok && used ? 0 : -1;
}
static int trace_port_open(void) {
    char name[96],device[48];dev_t expected=0;
    if(trace_read_attribute("/sys/class/virtio-ports/vport0p2/name",name,sizeof name) ||
       trace_read_attribute("/sys/class/virtio-ports/vport0p2/dev",device,sizeof device) ||
       trace_identity(name,device,&expected)) { errno=ENODEV;return -1; }
    const int fd=open("/dev/vport0p2",O_WRONLY|O_CLOEXEC|O_NOFOLLOW|O_NOCTTY);
    if(fd<0) return -1;
    struct stat metadata;
    if(fstat(fd,&metadata) || !S_ISCHR(metadata.st_mode) || metadata.st_rdev!=expected) {
        close(fd);errno=ENODEV;return -1;
    }
    return fd;
}
