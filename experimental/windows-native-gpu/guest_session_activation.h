// SPDX-License-Identifier: MIT
// Fresh root readiness acknowledgement before the same process execs Hyprland.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
struct wddm_session_activation {
    uint32_t magic;
    uint16_t version,bytes;
    uint32_t status,reserved;
    unsigned char nonce[32];
};
_Static_assert(sizeof(struct wddm_session_activation)==48,"fixed activation control packet");
#define WDDM_SESSION_ACTIVATE_REQUEST 0x51534157u
#define WDDM_SESSION_ACTIVATE_READY 0x52534157u

static inline uint64_t wddm_activation_now(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
static inline int wddm_activation_live(int process) {
    struct pollfd item={process,POLLIN,0};
    return poll(&item,1,0)==0 && !item.revents;
}
static inline int wddm_activation_wait(int socket_fd,int root,short events,uint64_t deadline) {
    for(;;) {
        const uint64_t now=wddm_activation_now();
        if(!now || now>=deadline) { errno=ETIMEDOUT;return -1; }
        struct pollfd items[2]={{socket_fd,events,0},{root,POLLIN,0}};
        const uint64_t remaining=(deadline-now+999999ull)/1000000ull;
        const int count=poll(items,root>=0?2:1,(int)(remaining<100?remaining:100));
        if(count<0 && errno==EINTR) continue;
        if(count<0) return -1;
        if(root>=0 && items[1].revents) { errno=EOWNERDEAD;return -1; }
        if(items[0].revents&events) return 0;
        if(items[0].revents) { errno=ECONNRESET;return -1; }
    }
}
static inline int wddm_activation_exchange_until(int socket_fd,uint64_t deadline) {
    const uint64_t start=wddm_activation_now();
    if(!start || start>=deadline) { errno=ETIMEDOUT;return -1; }
    if(deadline-start>10000000000ull) { errno=EINVAL;return -1; }
    int type=0,domain=0,root=-1;struct ucred peer;
    socklen_t bytes=sizeof type;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_TYPE,&type,&bytes) || bytes!=sizeof type || type!=SOCK_SEQPACKET) { errno=EPROTO;return -1; }
    bytes=sizeof domain;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_DOMAIN,&domain,&bytes) || bytes!=sizeof domain || domain!=AF_UNIX) { errno=EPROTO;return -1; }
    bytes=sizeof peer;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_PEERCRED,&peer,&bytes) || bytes!=sizeof peer ||
       peer.uid || peer.gid || peer.pid<=1 || peer.pid==getpid()) { errno=EPERM;return -1; }
    bytes=sizeof root;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_PEERPIDFD,&root,&bytes)) return -1;
    int result=-1,error=EIO;
    if(bytes!=sizeof root || root<0 || fcntl(root,F_SETFD,FD_CLOEXEC) || !wddm_activation_live(root)) goto done;
    struct wddm_session_activation request={WDDM_SESSION_ACTIVATE_REQUEST,1,sizeof request,0,0,{0}};
    size_t offset=0;
    while(offset<sizeof request.nonce) {
        const ssize_t count=getrandom(request.nonce+offset,sizeof request.nonce-offset,GRND_NONBLOCK);
        if(count<0 && errno==EINTR) {
            if(wddm_activation_now()>=deadline) { error=ETIMEDOUT;goto done; }
            continue;
        }
        if(count<=0) { error=errno?errno:EIO;goto done; }
        offset+=(size_t)count;
    }
    for(;;) {
        if(wddm_activation_wait(socket_fd,root,POLLOUT,deadline)) { error=errno;goto done; }
        const ssize_t count=send(socket_fd,&request,sizeof request,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if(count!=(ssize_t)sizeof request) { error=count<0?errno:EPROTO;goto done; }
        break;
    }
    for(;;) {
        if(wddm_activation_wait(socket_fd,root,POLLIN,deadline)) { error=errno;goto done; }
        struct wddm_session_activation reply;
        struct iovec data={&reply,sizeof reply};
        union { struct cmsghdr aligned;char bytes[CMSG_SPACE(8*sizeof(int))]; } control;
        memset(&control,0,sizeof control);
        struct msghdr message;memset(&message,0,sizeof message);
        message.msg_iov=&data;message.msg_iovlen=1;message.msg_control=control.bytes;message.msg_controllen=sizeof control.bytes;
        const ssize_t count=recvmsg(socket_fd,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if(count<0 && (errno==EINTR || errno==EAGAIN)) continue;
        int ancillary=0;
        for(struct cmsghdr* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
            ancillary=1;
            if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
                const size_t descriptors=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
                for(size_t index=0;index<descriptors;++index) {
                    int descriptor=-1;memcpy(&descriptor,(char*)CMSG_DATA(c)+index*sizeof(int),sizeof descriptor);
                    if(descriptor>=0) { const int closed=close(descriptor);(void)closed; }
                }
            }
        }
        if(count!=(ssize_t)sizeof reply || ancillary || (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) ||
           reply.magic!=WDDM_SESSION_ACTIVATE_READY || reply.version!=1 || reply.bytes!=sizeof reply ||
           reply.status || reply.reserved || memcmp(reply.nonce,request.nonce,sizeof reply.nonce)) { error=EPROTO;goto done; }
        if(!wddm_activation_live(root)) { error=EOWNERDEAD;goto done; }
        result=0;error=0;break;
    }
done:
    if(root>=0 && close(root)) { if(!error) error=errno;result=-1; }
    errno=error;return result;
}

static inline int wddm_activation_exchange(int socket_fd,unsigned timeout_ms) {
    if(!timeout_ms || timeout_ms>10000) { errno=EINVAL;return -1; }
    const uint64_t start=wddm_activation_now();
    if(!start || start>UINT64_MAX-(uint64_t)timeout_ms*1000000ull) { errno=EIO;return -1; }
    return wddm_activation_exchange_until(socket_fd,start+(uint64_t)timeout_ms*1000000ull);
}
