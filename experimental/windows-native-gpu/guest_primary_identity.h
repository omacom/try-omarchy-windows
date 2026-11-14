// SPDX-License-Identifier: MIT
// Root supervisor -> broker only. This is not an ordinary client operation.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

struct wddm_primary_identity {
    uint32_t magic;
    uint16_t version, bytes;
    uint64_t pid, reserved[2];
};

static inline int wddm_primary_live(int process) {
    struct pollfd status={process,POLLIN,0};
    int count;
    do { count=poll(&status,1,0); } while(count<0 && errno==EINTR);
    return count==0 && !status.revents;
}

static inline int wddm_primary_channel(int channel) {
    int type=0,domain=0;struct ucred owner;
    socklen_t size=sizeof type;
    if(getsockopt(channel,SOL_SOCKET,SO_TYPE,&type,&size) || size!=sizeof type || type!=SOCK_SEQPACKET) return -1;
    size=sizeof domain;
    if(getsockopt(channel,SOL_SOCKET,SO_DOMAIN,&domain,&size) || size!=sizeof domain || domain!=AF_UNIX) return -1;
    size=sizeof owner;
    if(getsockopt(channel,SOL_SOCKET,SO_PEERCRED,&owner,&size) || size!=sizeof owner || owner.uid || owner.gid || owner.pid<=0) return -1;
    return 0;
}

// Read the kernel's PID for a pidfd, never reopen a numeric PID to authenticate
// the receiver's peer. The live checks also reject an exited/recycled identity.
static inline int wddm_primary_pidfd_pid(int process,uint64_t expected) {
    if(!expected || expected>INT_MAX || !wddm_primary_live(process) ||
       syscall(SYS_pidfd_send_signal,process,0,NULL,0)) return -1;
    char path[64],data[512];
    int n=snprintf(path,sizeof path,"/proc/self/fdinfo/%d",process);
    if(n<=0 || (size_t)n>=sizeof path) return -1;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -1;
    ssize_t count;
    do { count=read(fd,data,sizeof data-1); } while(count<0 && errno==EINTR);
    close(fd);
    if(count<=0 || count==(ssize_t)sizeof data-1) return -1;
    data[count]=0;
    const char* field=strstr(data,"\nPid:\t");
    if(!field) return -1;
    field+=6;
    uint64_t pid=0;
    if(*field<'0' || *field>'9') return -1;
    while(*field>='0' && *field<='9') {
        pid=pid*10+(unsigned)(*field++-'0');
        if(pid>INT_MAX) return -1;
    }
    return pid==expected && *field=='\n' && wddm_primary_live(process) ? 0 : -1;
}

static inline int wddm_primary_send(int channel,pid_t pid,int process) {
    if(getuid() || geteuid() || wddm_primary_channel(channel) ||
       pid<=0 || wddm_primary_pidfd_pid(process,(uint64_t)pid)) { errno=EPERM;return -1; }
    struct wddm_primary_identity identity={0x49504257u,1,sizeof identity,(uint64_t)pid,{0,0}};
    struct iovec data={&identity,sizeof identity};
    union { struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control,0,sizeof control);
    struct msghdr message;memset(&message,0,sizeof message);
    message.msg_iov=&data;message.msg_iovlen=1;message.msg_control=control.bytes;message.msg_controllen=sizeof control.bytes;
    struct cmsghdr* rights=CMSG_FIRSTHDR(&message);
    rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(rights),&process,sizeof process);
    ssize_t count;
    do { count=sendmsg(channel,&message,MSG_NOSIGNAL|MSG_DONTWAIT); } while(count<0 && errno==EINTR);
    return count==(ssize_t)sizeof identity ? 0 : -1;
}

// On error close every received descriptor, including malformed/truncated
// messages. The output pidfd is owned by the caller only after success.
static inline int wddm_primary_receive(int channel,int timeout_ms,uint64_t* pid,int* process) {
    if(wddm_primary_channel(channel) || timeout_ms<0 || timeout_ms>30000) { errno=EPERM;return -1; }
    struct pollfd ready={channel,POLLIN,0};
    int available;
    do { available=poll(&ready,1,timeout_ms); } while(available<0 && errno==EINTR);
    if(available!=1 || !(ready.revents&POLLIN)) { errno=ETIMEDOUT;return -1; }
    struct wddm_primary_identity identity;
    struct iovec data={&identity,sizeof identity};
    union { struct cmsghdr alignment;char bytes[CMSG_SPACE(4*sizeof(int))]; } control;
    memset(&control,0,sizeof control);
    struct msghdr message;memset(&message,0,sizeof message);
    message.msg_iov=&data;message.msg_iovlen=1;message.msg_control=control.bytes;message.msg_controllen=sizeof control.bytes;
    ssize_t count;
    do { count=recvmsg(channel,&message,MSG_CMSG_CLOEXEC|MSG_DONTWAIT); } while(count<0 && errno==EINTR);
    int received[4],fds=0,invalid=0;
    for(struct cmsghdr* item=CMSG_FIRSTHDR(&message);item;item=CMSG_NXTHDR(&message,item)) {
        if(item->cmsg_level!=SOL_SOCKET || item->cmsg_type!=SCM_RIGHTS || item->cmsg_len<CMSG_LEN(0)) { invalid=1;continue; }
        size_t bytes=item->cmsg_len-CMSG_LEN(0);
        if(bytes%sizeof(int)) invalid=1;
        for(size_t offset=0;offset+sizeof(int)<=bytes;offset+=sizeof(int)) {
            int fd=-1;memcpy(&fd,CMSG_DATA(item)+offset,sizeof fd);
            if(fds<4) received[fds++]=fd;else { close(fd);invalid=1; }
        }
    }
    if(count!=(ssize_t)sizeof identity || (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || invalid || fds!=1 ||
       identity.magic!=0x49504257u || identity.version!=1 || identity.bytes!=sizeof identity ||
       identity.reserved[0] || identity.reserved[1] || wddm_primary_pidfd_pid(received[0],identity.pid)) {
        for(int i=0;i<fds;++i) close(received[i]);
        errno=EPROTO;return -1;
    }
    *pid=identity.pid;*process=received[0];return 0;
}
