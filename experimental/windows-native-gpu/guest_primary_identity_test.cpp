// SPDX-License-Identifier: MIT
// Real root credentials, Linux pidfds and /proc. No GPU result is claimed.
#include "guest_session_process.h"
#include <grp.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>

#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(0)
struct Descriptor {
    int fd;
    explicit Descriptor(int value):fd(value) { CHECK(fd>=0); }
    ~Descriptor() { close(fd); }
};
struct Child {
    pid_t pid;int release;
    Child() {
        int ready[2],finish[2];CHECK(!pipe2(ready,O_CLOEXEC));CHECK(!pipe2(finish,O_CLOEXEC));
        pid=fork();CHECK(pid>=0);
        if(!pid) {
            alarm(20);close(ready[0]);close(finish[1]);
            if(setgroups(0,NULL) || setgid(65534) || setuid(65534) || prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)) _exit(126);
            char value='R';if(write(ready[1],&value,1)!=1) _exit(127);
            if(read(finish[0],&value,1)!=1) _exit(127);
            _exit(0);
        }
        close(ready[1]);close(finish[0]);release=finish[1];
        char value=0;CHECK(read(ready[0],&value,1)==1 && value=='R');close(ready[0]);
    }
    void stop() {
        if(pid<=0) return;
        CHECK(write(release,"X",1)==1);close(release);release=-1;
        int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));pid=-1;
    }
    ~Child() { if(pid>0) { kill(pid,SIGKILL);waitpid(pid,NULL,0);close(release); } }
};
static unsigned descriptors() {
    DIR* directory=opendir("/proc/self/fd");CHECK(directory);
    unsigned count=0;while(readdir(directory)) ++count;closedir(directory);return count;
}
static void raw(int channel,const wddm_primary_identity& identity,int process,unsigned count,size_t bytes=sizeof(wddm_primary_identity)) {
    unsigned char packet[sizeof identity+1]={0};memcpy(packet,&identity,sizeof identity);
    iovec data{packet,bytes};
    union { cmsghdr alignment;char data[CMSG_SPACE(5*sizeof(int))]; } control{};
    msghdr message{};message.msg_iov=&data;message.msg_iovlen=1;
    if(count) {
        message.msg_control=control.data;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        auto* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(count*sizeof(int));
        for(unsigned i=0;i<count;++i) memcpy(CMSG_DATA(rights)+i*sizeof(int),&process,sizeof process);
    }
    CHECK(sendmsg(channel,&message,MSG_NOSIGNAL)==static_cast<ssize_t>(bytes));
}
int main() {
    try {
        CHECK(!getuid() && !geteuid());alarm(30);
        CHECK(sizeof(wddm_primary_identity)==32);
        Child child;Descriptor pinned(static_cast<int>(syscall(SYS_pidfd_open,child.pid,0)));
        CHECK(!wddm_primary_pidfd_pid(pinned.fd,static_cast<uint64_t>(child.pid)));
        CHECK(wddm_primary_pidfd_pid(pinned.fd,static_cast<uint64_t>(getpid())));
        int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));
        Descriptor sender(pair[0]),receiver(pair[1]);uint64_t pid=0;int process=-1;
        CHECK(!wddm_primary_send(sender.fd,child.pid,pinned.fd));
        CHECK(!wddm_primary_receive(receiver.fd,100,&pid,&process));Descriptor accepted(process);
        CHECK(pid==static_cast<uint64_t>(child.pid) && (fcntl(accepted.fd,F_GETFD)&FD_CLOEXEC));
        CHECK(wddm_primary_receive(receiver.fd,0,&pid,&process));
        const unsigned before=descriptors();
        const wddm_primary_identity original{0x49504257u,1,32,static_cast<uint64_t>(child.pid),{0,0}};
        for(unsigned mode=0;mode<12;++mode) {
            auto identity=original;unsigned fds=1;size_t bytes=sizeof identity;
            int fd=pinned.fd;Descriptor unrelated(open("/dev/null",O_RDONLY|O_CLOEXEC));
            switch(mode) {
                case 0: ++identity.magic;break;
                case 1: ++identity.version;break;
                case 2: --identity.bytes;break;
                case 3: identity.pid=0;break;
                case 4: identity.pid=static_cast<uint64_t>(getpid());break;
                case 5: identity.reserved[0]=1;break;
                case 6: identity.reserved[1]=1;break;
                case 7: fds=0;break;
                case 8: fds=2;break;
                case 9: fds=5;break; // MSG_CTRUNC must not leak received FDs.
                case 10: ++bytes;break; // MSG_TRUNC.
                case 11: fd=unrelated.fd;break;
            }
            raw(sender.fd,identity,fd,fds,bytes);
            CHECK(wddm_primary_receive(receiver.fd,100,&pid,&process));
        }
        CHECK(descriptors()==before);
        int stream[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,stream));
        CHECK(wddm_primary_channel(stream[0]));close(stream[0]);close(stream[1]);
        CHECK(!setegid(65534) && !seteuid(65534));
        int ordinary[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ordinary));
        CHECK(!seteuid(0) && !setegid(0));
        CHECK(wddm_primary_channel(ordinary[0]) && wddm_primary_channel(ordinary[1]));close(ordinary[0]);close(ordinary[1]);

        char group[4096];CHECK(!wddm_session_read(getpid(),"cgroup",group,sizeof group));
        CHECK(!strncmp(group,"0::/",4));std::string cgroup=group+3;CHECK(cgroup.back()=='\n');cgroup.pop_back();
        pid_t selected=0;Descriptor found(wddm_session_find("/proc/self/exe",cgroup.c_str(),65534,65534,&selected));
        CHECK(selected==child.pid && !wddm_primary_pidfd_pid(found.fd,static_cast<uint64_t>(selected)));
        CHECK(wddm_session_find("/proc/self/exe","/wrong-service",65534,65534,&selected)<0);
        CHECK(wddm_session_find("/proc/self/exe",cgroup.c_str(),65533,65534,&selected)<0);
        CHECK(wddm_session_find("/usr/bin/true",cgroup.c_str(),65534,65534,&selected)<0);
        { Child other;CHECK(wddm_session_find("/proc/self/exe",cgroup.c_str(),65534,65534,&selected)<0 && errno==ENOTUNIQ); }
        CHECK(wddm_primary_send(sender.fd,getpid(),pinned.fd));
        CHECK(!wddm_primary_send(sender.fd,child.pid,pinned.fd));
        child.stop();
        CHECK(!wddm_primary_live(pinned.fd));
        CHECK(wddm_primary_receive(receiver.fd,100,&pid,&process));
        CHECK(wddm_primary_send(sender.fd,static_cast<pid_t>(original.pid),pinned.fd));
        CHECK(wddm_session_find("/proc/self/exe",cgroup.c_str(),65534,65534,&selected)<0);
        std::cout<<"PRIMARY_IDENTITY rootCredentials=true actualPidfdTransfer=true malformedAndExitedRejected=true fdLeaks=0 exactServiceExecutableUid=true ambiguousRejected=true gpu=false\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<" errno="<<errno<<'\n';return 1; }
}
