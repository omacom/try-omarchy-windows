// SPDX-License-Identifier: MIT
// CPU-only exercise of the actual activation client, with live FD accounting.
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/un.h>
#include "guest_session_activation.h"
static unsigned count_fds(void) {
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;
    for(struct dirent* entry;(entry=readdir(directory));)
        if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(!closedir(directory));return count;
}
int main(int argc,char** argv) {
    assert(argc==4);
    struct sockaddr_un address={.sun_family=AF_UNIX};assert(strlen(argv[1])<sizeof address.sun_path);
    strcpy(address.sun_path,argv[1]);
    const int socket_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(socket_fd>=0);
    assert(!connect(socket_fd,(struct sockaddr*)&address,sizeof address));
    const unsigned before=count_fds();
    const int result=wddm_activation_exchange(socket_fd,(unsigned)atoi(argv[2]));
    const int error=errno;
    assert(count_fds()==before);
    assert(atoi(argv[3])?(result==-1):(result==0));
    assert(!close(socket_fd));
    printf("ACTIVATION_CPU_CONTROL result=%d errno=%d fdBalancedBeforeExit=true gpuUsed=false\n",result,error);
    return 0;
}
