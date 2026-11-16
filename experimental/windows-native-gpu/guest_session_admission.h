// SPDX-License-Identifier: MIT
// Root admission of the live wrapper that will exec the compositor in place.
#pragma once
#include "guest_session_activation.h"
#include "guest_session_process.h"
#include <stdlib.h>

struct wddm_session_peer { pid_t pid;int process;struct wddm_session_activation request; };
static inline int wddm_session_zero_caps(const char* status) {
    const char* fields[]={"\nCapPrm:\t","\nCapEff:\t"};
    for(unsigned index=0;index<2;++index) {
        const char* at=strstr(status,fields[index]);
        if(!at) return 0;
        at+=strlen(fields[index]);
        unsigned digits=0;
        while(*at=='0') { ++at;++digits; }
        if(!digits || digits>16 || *at!='\n') return 0;
    }
    return 1;
}
static inline int wddm_session_pin(int socket_fd,uid_t uid,gid_t gid,int executable,const char* cgroup,struct wddm_session_peer* peer) {
    peer->process=-1;peer->pid=0;
    if(getuid() || geteuid() || !uid || !gid || !cgroup || cgroup[0]!='/' || strchr(cgroup,'\n')) { errno=EPERM;return -1; }
    struct stat image;
    if(fstat(executable,&image) || !S_ISREG(image.st_mode) || image.st_uid || (image.st_mode&0022)) { errno=EPERM;return -1; }
    struct ucred credentials;socklen_t bytes=sizeof credentials;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_PEERCRED,&credentials,&bytes) || bytes!=sizeof credentials ||
       credentials.uid!=uid || credentials.gid!=gid || credentials.pid<=1 || credentials.pid==getpid()) { errno=EPERM;return -1; }
    int process=-1;bytes=sizeof process;
    if(getsockopt(socket_fd,SOL_SOCKET,SO_PEERPIDFD,&process,&bytes)) return -1;
    int result=-1;
    if(bytes!=sizeof process || process<0 || fcntl(process,F_SETFD,FD_CLOEXEC) ||
       wddm_primary_pidfd_pid(process,(uint64_t)credentials.pid)) goto done;
    char path[64],membership[4096],status[8192],wanted[4096];struct stat actual,after;
    const int n=snprintf(path,sizeof path,"/proc/%ld/exe",(long)credentials.pid);
    const int size=snprintf(wanted,sizeof wanted,"0::%s\n",cgroup);
    if(n<=0 || (size_t)n>=sizeof path || size<=0 || (size_t)size>=sizeof wanted ||
       stat(path,&actual) || actual.st_dev!=image.st_dev || actual.st_ino!=image.st_ino ||
       wddm_session_read(credentials.pid,"cgroup",membership,sizeof membership) || strcmp(membership,wanted) ||
       wddm_session_read(credentials.pid,"status",status,sizeof status) ||
       !wddm_session_credentials(status,uid,gid) || !wddm_session_zero_caps(status) ||
       stat(path,&after) || after.st_dev!=image.st_dev || after.st_ino!=image.st_ino ||
       wddm_primary_pidfd_pid(process,(uint64_t)credentials.pid)) goto done;
    peer->pid=credentials.pid;peer->process=process;process=-1;result=0;
done:
    if(process>=0) { const int closed=close(process);(void)closed; }
    if(result) errno=EPERM;
    return result;
}
static inline int wddm_session_wait(int fd,int peer,int stop,short events,uint64_t deadline) {
    for(;;) {
        const uint64_t now=wddm_activation_now();
        if(!now || now>=deadline) { errno=ETIMEDOUT;return -1; }
        struct pollfd items[3]={{fd,events,0},{peer,POLLIN,0},{stop,POLLIN,0}};
        const uint64_t remaining=(deadline-now+999999ull)/1000000ull;
        const int count=poll(items,stop>=0?3:2,(int)(remaining<100?remaining:100));
        if(count<0 && errno==EINTR) continue;
        if(count<0) return -1;
        if(items[2].revents) { errno=ECANCELED;return -1; }
        if(items[1].revents) { errno=ESRCH;return -1; }
        if(items[0].revents&events) return 0;
        if(items[0].revents) { errno=ECONNRESET;return -1; }
    }
}
static inline int wddm_session_receive(int fd,struct wddm_session_peer* peer,int stop,uint64_t deadline) {
    for(;;) {
        if(wddm_session_wait(fd,peer->process,stop,POLLIN,deadline)) return -1;
        memset(&peer->request,0,sizeof peer->request);
        struct iovec data={&peer->request,sizeof peer->request};
        union { struct cmsghdr aligned;char bytes[CMSG_SPACE(8*sizeof(int))]; } control;
        memset(&control,0,sizeof control);
        struct msghdr message;memset(&message,0,sizeof message);
        message.msg_iov=&data;message.msg_iovlen=1;message.msg_control=control.bytes;message.msg_controllen=sizeof control.bytes;
        const ssize_t count=recvmsg(fd,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if(count<0 && (errno==EAGAIN || errno==EINTR)) continue;
        int ancillary=0;
        for(struct cmsghdr* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
            ancillary=1;
            if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
                const size_t descriptors=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
                for(size_t index=0;index<descriptors;++index) {
                    int received=-1;memcpy(&received,(char*)CMSG_DATA(c)+index*sizeof(int),sizeof received);
                    if(received>=0) { const int closed=close(received);(void)closed; }
                }
            }
        }
        unsigned nonzero=0;
        for(unsigned index=0;index<sizeof peer->request.nonce;++index) nonzero|=peer->request.nonce[index];
        if(count!=(ssize_t)sizeof peer->request || ancillary || (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) ||
           peer->request.magic!=WDDM_SESSION_ACTIVATE_REQUEST || peer->request.version!=1 ||
           peer->request.bytes!=sizeof peer->request || peer->request.status || peer->request.reserved || !nonzero ||
           !wddm_primary_live(peer->process)) { errno=EPROTO;return -1; }
        return 0;
    }
}
static inline int wddm_session_reply(int fd,struct wddm_session_peer* peer,int stop,uint64_t deadline) {
    struct wddm_session_activation ready=peer->request;ready.magic=WDDM_SESSION_ACTIVATE_READY;
    for(;;) {
        if(wddm_session_wait(fd,peer->process,stop,POLLOUT,deadline)) return -1;
        const ssize_t count=send(fd,&ready,sizeof ready,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if(count!=(ssize_t)sizeof ready) { errno=EPROTO;return -1; }
        return 0;
    }
}
