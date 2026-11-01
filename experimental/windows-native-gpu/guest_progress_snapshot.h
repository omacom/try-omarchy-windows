/* SPDX-License-Identifier: MIT */
/* Read-only, bounded diagnostic data. File ownership is not process attestation. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WDDM_PROGRESS_SNAPSHOT_MAX 512u

static int wddm_progress_snapshot(const char* path,uid_t expected_uid,
                                 unsigned char output[WDDM_PROGRESS_SNAPSHOT_MAX],size_t* bytes) {
    *bytes=0;
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return -1;
    struct stat before,after;
    int error=0;
    size_t used=0;
    unsigned char buffer[WDDM_PROGRESS_SNAPSHOT_MAX+1];
    if(fstat(fd,&before)) error=errno;
    else if(!S_ISREG(before.st_mode) || before.st_uid!=expected_uid) error=EPERM;
    else if(before.st_size<=0 || before.st_size>WDDM_PROGRESS_SNAPSHOT_MAX) error=EMSGSIZE;
    while(!error && used<sizeof buffer) {
        const ssize_t count=read(fd,buffer+used,sizeof buffer-used);
        if(count<0) { if(errno==EINTR) continue;error=errno;break; }
        if(!count) break;
        used+=(size_t)count;
    }
    if(!error && (used!=((size_t)before.st_size) || used>WDDM_PROGRESS_SNAPSHOT_MAX)) error=EMSGSIZE;
    if(!error && fstat(fd,&after)) error=errno;
    if(!error && (after.st_size!=before.st_size || after.st_uid!=before.st_uid ||
        after.st_mtim.tv_sec!=before.st_mtim.tv_sec || after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec!=before.st_ctim.tv_sec || after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec)) error=EAGAIN;
    if(close(fd) && !error) error=errno;
    if(error) { errno=error;return -1; }
    for(size_t index=0;index<used;++index) output[index]=buffer[index];
    *bytes=used;
    return 0;
}
