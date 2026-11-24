// SPDX-License-Identifier: MIT
// Root-only discovery for a configured systemd user compositor unit. Never
// accept a PID, executable path or service name supplied by a GPU client.
#pragma once
#include "guest_primary_identity.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

static inline int wddm_session_read(pid_t pid,const char* name,char* data,size_t capacity) {
    char path[96];
    int n=snprintf(path,sizeof path,"/proc/%ld/%s",(long)pid,name);
    if(n<=0 || (size_t)n>=sizeof path || capacity<2) return -1;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -1;
    size_t bytes=0;
    while(bytes<capacity-1) {
        ssize_t count=read(fd,data+bytes,capacity-1-bytes);
        if(count<0 && errno==EINTR) continue;
        if(count<0) { close(fd);return -1; }
        if(!count) break;
        bytes+=(size_t)count;
    }
    close(fd);
    if(bytes==capacity-1) return -1;
    data[bytes]=0;return 0;
}

static inline int wddm_session_credentials(const char* status,uid_t uid,gid_t gid) {
    const char* users=strstr(status,"\nUid:");const char* groups=strstr(status,"\nGid:");
    unsigned u[4],g[4];
    return users && groups && sscanf(users,"\nUid:\t%u %u %u %u",&u[0],&u[1],&u[2],&u[3])==4 &&
        sscanf(groups,"\nGid:\t%u %u %u %u",&g[0],&g[1],&g[2],&g[3])==4 &&
        u[0]==uid && u[1]==uid && u[2]==uid && u[3]==uid &&
        g[0]==gid && g[1]==gid && g[2]==gid && g[3]==gid;
}

// Returns an owned live pidfd only for one unique exact executable/cgroup/UID
// match. The pinned process must stay alive through all /proc observations.
static inline int wddm_session_find(const char* executable,const char* group,uid_t uid,gid_t gid,pid_t* selected) {
    if(getuid() || geteuid() || !uid || !gid || executable[0]!='/' || group[0]!='/' || strchr(group,'\n')) { errno=EPERM;return -1; }
    int expected=open(executable,O_PATH|O_CLOEXEC);
    struct stat image;
    if(expected<0) return -1;
    int trusted=!fstat(expected,&image) && S_ISREG(image.st_mode) && !image.st_uid && !(image.st_mode&0022);
    if(!trusted) { close(expected);errno=EPERM;return -1; }
    char wanted[4096];int n=snprintf(wanted,sizeof wanted,"0::%s\n",group);
    if(n<=0 || (size_t)n>=sizeof wanted) { close(expected);errno=EINVAL;return -1; }
    DIR* directory=opendir("/proc");
    if(!directory) { close(expected);return -1; }
    int found=-1,ambiguous=0;unsigned entries=0;
    struct dirent* entry;
    while((entry=readdir(directory))) {
        if(++entries>32768) { ambiguous=1;break; }
        const char* name=entry->d_name;
        if(!*name || strspn(name,"0123456789")!=strlen(name)) continue;
        unsigned long number=strtoul(name,NULL,10);
        if(!number || number>INT_MAX) continue;
        pid_t pid=(pid_t)number;
        int process=(int)syscall(SYS_pidfd_open,pid,0);
        if(process<0) continue;
        char path[64];n=snprintf(path,sizeof path,"/proc/%ld/exe",(long)pid);
        struct stat actual;char membership[4096],status[8192];
        if(n<=0 || (size_t)n>=sizeof path || stat(path,&actual) || actual.st_dev!=image.st_dev || actual.st_ino!=image.st_ino ||
           wddm_session_read(pid,"cgroup",membership,sizeof membership) || strcmp(membership,wanted) ||
           wddm_session_read(pid,"status",status,sizeof status) || !wddm_session_credentials(status,uid,gid) ||
           wddm_primary_pidfd_pid(process,(uint64_t)pid)) { close(process);continue; }
        if(found>=0) { close(process);ambiguous=1;break; }
        found=process;*selected=pid;
    }
    closedir(directory);close(expected);
    if(ambiguous) { if(found>=0) close(found);errno=ENOTUNIQ;return -1; }
    if(found<0) errno=EAGAIN;
    return found;
}
