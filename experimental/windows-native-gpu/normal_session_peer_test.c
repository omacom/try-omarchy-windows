// SPDX-License-Identifier: MIT
// Real wrapper by default; explicit CPU modes inject malformed activation data.
#define _GNU_SOURCE
#define main normal_wrapper_main
#include "hyprland_session_exec.c"
#undef main
#include <assert.h>
int main(int argc,char** argv) {
    const char* mode=getenv("ROOT_SERVICE_PEER_TEST");
    if(!mode) return normal_wrapper_main(argc,argv);
    const int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(fd>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,WDDM_SESSION_ACTIVATION_SOCKET);
    assert(!connect(fd,(struct sockaddr*)&address,sizeof address));
    if(!strcmp(mode,"idle")) {
        struct pollfd closed={fd,POLLIN,0};assert(poll(&closed,1,3000)==1);char byte;assert(recv(fd,&byte,1,0)==0);close(fd);return 0;
    }
    struct wddm_session_activation request={WDDM_SESSION_ACTIVATE_REQUEST,1,sizeof request,0,0,{1}};
    if(!strcmp(mode,"magic")) ++request.magic;
    if(!strcmp(mode,"version")) ++request.version;
    if(!strcmp(mode,"size")) ++request.bytes;
    if(!strcmp(mode,"status")) ++request.status;
    if(!strcmp(mode,"reserved")) ++request.reserved;
    if(!strcmp(mode,"nonce")) memset(request.nonce,0,sizeof request.nonce);
    unsigned char data[sizeof request+1];memcpy(data,&request,sizeof request);data[sizeof request]=1;
    const size_t bytes=!strcmp(mode,"short")?sizeof request-1:!strcmp(mode,"long")?sizeof request+1:sizeof request;
    int pipes[2]={-1,-1};
    if(!strcmp(mode,"rights") || !strcmp(mode,"truncated-rights")) {
        assert(!pipe2(pipes,O_CLOEXEC));
        int rights[40];const unsigned count=!strcmp(mode,"rights")?1:40;
        for(unsigned n=0;n<count;++n) rights[n]=pipes[1];
        union { struct cmsghdr aligned;char bytes[CMSG_SPACE(sizeof rights)]; } control;memset(&control,0,sizeof control);
        struct iovec item={data,bytes};struct msghdr message={0};message.msg_iov=&item;message.msg_iovlen=1;
        message.msg_control=control.bytes;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(c),rights,count*sizeof(int));assert(sendmsg(fd,&message,MSG_NOSIGNAL)==(ssize_t)bytes);
        assert(!close(pipes[0]) && !close(pipes[1]));
    } else assert(send(fd,data,bytes,MSG_NOSIGNAL)==(ssize_t)bytes);
    struct pollfd closed={fd,POLLIN,0};assert(poll(&closed,1,3000)==1);
    char byte;assert(recv(fd,&byte,1,0)==0);assert(!close(fd));return 0;
}
