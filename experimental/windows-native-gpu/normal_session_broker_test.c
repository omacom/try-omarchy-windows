// SPDX-License-Identifier: MIT
// CPU-only broker: actual inherited readiness + trusted pidfd registration.
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "guest_primary_identity.h"
#ifndef CPU_BROKER_MODE
#define CPU_BROKER_MODE 0
#endif
int main(int argc,char** argv) {
    assert(argc==8 && getuid()==0 && getgid()==0 && !strcmp(argv[6],"--coordinator"));
    assert(getenv("WDDM_BRIDGE_BROKER_SESSION") && !strcmp(getenv("WDDM_BRIDGE_BROKER_SESSION"),"1"));
    assert(getenv("WDDM_BRIDGE_RETIREMENT_WITNESSES") && !strcmp(getenv("WDDM_BRIDGE_RETIREMENT_WITNESSES"),"0"));
    assert(getenv("WDDM_BRIDGE_COORDINATED_REFERENCES") && getenv("WDDM_BRIDGE_COORDINATED_RETIREMENT"));
#ifdef CPU_BROKER_WITNESS
    const int witness=open(CPU_BROKER_WITNESS,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    assert(witness>=0 && write(witness,"S",1)==1 && !close(witness));
#endif
    const int ready=atoi(argv[5]),identity=atoi(argv[7]);
    assert(ready>=3 && identity>=3 && ready!=identity && !wddm_primary_channel(identity));
    sigset_t inherited;assert(!sigprocmask(SIG_SETMASK,NULL,&inherited));
    assert(!sigismember(&inherited,SIGTERM) && !sigismember(&inherited,SIGINT));
    DIR* descriptors=opendir("/proc/self/fd");assert(descriptors);
    for(struct dirent* entry;(entry=readdir(descriptors));) {
        if(strspn(entry->d_name,"0123456789")!=strlen(entry->d_name)) continue;
        const int fd=atoi(entry->d_name);
        assert(fd<=2 || fd==ready || fd==identity || fd==dirfd(descriptors));
    }
    assert(!closedir(descriptors));
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listener>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};assert(strlen(argv[1])<sizeof address.sun_path);strcpy(address.sun_path,argv[1]);
    assert(!bind(listener,(struct sockaddr*)&address,sizeof address));
    assert(!chown(argv[1],(uid_t)atoi(argv[3]),(gid_t)atoi(argv[4])) && !chmod(argv[1],0600));
    if(CPU_BROKER_MODE==1) return 1;
    if(CPU_BROKER_MODE==2) { for(;;) pause(); }
    if(CPU_BROKER_MODE==3) { assert(write(ready,"X",1)==1);for(;;) pause(); }
    assert(write(ready,"R",1)==1 && !close(ready));
    uint64_t pid=0;int process=-1;
    assert(!wddm_primary_receive(identity,30000,&pid,&process) && process>=0);
    assert(!close(identity));
    if(CPU_BROKER_MODE==4) return 1;
    struct pollfd exited={process,POLLIN,0};assert(poll(&exited,1,30000)==1);
    assert(!wddm_primary_live(process));
    if(CPU_BROKER_MODE==5) { for(;;) pause(); }
    assert(!close(process) && !close(listener));return 0;
}
