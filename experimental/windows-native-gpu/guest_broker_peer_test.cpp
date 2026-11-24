// SPDX-License-Identifier: MIT
// Real Linux processes/sockets/pidfds. No fake peer credentials or GPU claims.
#include "guest_broker_peer.h"
#include <chrono>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>

using namespace guest_broker;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("Broker contract: ") + #x); } while (0)
static void byte(int fd, char value) { CHECK(::write(fd, &value, 1) == 1); }
static char readByte(int fd) {
    pollfd ready{fd, POLLIN, 0}; CHECK(poll(&ready, 1, 5000) == 1);
    char value = 0; CHECK(::read(fd, &value, 1) == 1); return value;
}
static unsigned fdCount() {
    auto* dir = opendir("/proc/self/fd"); CHECK(dir);
    unsigned count = 0;
    while (auto* entry = readdir(dir)) if (entry->d_name[0] != '.') ++count;
    closedir(dir); return count;
}
static void closeInherited(int socket, int memory, int commands, int notices) {
    auto* dir=opendir("/proc/self/fd"); CHECK(dir);
    while (auto* entry=readdir(dir)) {
        if (entry->d_name[0]=='.') continue;
        const auto fd=std::atoi(entry->d_name);
        if (fd>2 && fd!=dirfd(dir) && fd!=socket && fd!=memory && fd!=commands && fd!=notices) ::close(fd);
    }
    closedir(dir);
}
static Received receive(Peer& peer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto result = peer.receive();
        if (result.state != ReceiveState::Idle) return result;
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void exited(Peer& peer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!peer.processExited()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
enum class Mode { Good, Reset, Finalizer, Descendant, Leader, Magic, Version, Reserved, Length,
                  FdCount, Sequence, Operation, Oversize, ExcessFds, Empty, Replay, Status };
static void sendFrame(int socket, int backing, Mode mode) {
    Frame f; f.operation = Operation::DriverCall; f.bytes = 4; f.fds = 1;
    if (mode == Mode::Magic) ++f.magic;
    if (mode == Mode::Version) ++f.version;
    if (mode == Mode::Reserved) f.reserved = 1;
    if (mode == Mode::Status) f.status = EPERM;
    if (mode == Mode::Length) f.bytes = 3;
    if (mode == Mode::FdCount) f.fds = 0;
    if (mode == Mode::Sequence) f.sequence = 0;
    if (mode == Mode::Operation) f.operation = static_cast<Operation>(99);
    const unsigned descriptors = mode == Mode::ExcessFds ? 9 : 1;
    f.fds = mode == Mode::ExcessFds ? 9 : f.fds;
    std::vector<std::uint8_t> packet(sizeof(f) + (mode == Mode::Oversize ? MaxPayload+1 : 4), 65);
    std::memcpy(packet.data(), &f, sizeof(f));
    if (mode == Mode::Empty) packet.clear();
    iovec vec{packet.data(), packet.size()};
    union { cmsghdr alignment; char data[CMSG_SPACE(9*sizeof(int))]; } control{};
    msghdr message{}; message.msg_iov=&vec; message.msg_iovlen=1;
    message.msg_control=control.data; message.msg_controllen=CMSG_SPACE(descriptors*sizeof(int));
    auto* header=CMSG_FIRSTHDR(&message); header->cmsg_level=SOL_SOCKET; header->cmsg_type=SCM_RIGHTS;
    header->cmsg_len=CMSG_LEN(descriptors*sizeof(int));
    for (unsigned n=0;n<descriptors;++n) std::memcpy(CMSG_DATA(header)+n*sizeof(int), &backing, sizeof(backing));
    CHECK(sendmsg(socket,&message,MSG_NOSIGNAL)==static_cast<ssize_t>(packet.size()));
}
struct Worker { int socket, backing, commands, notify; pid_t leader; };
static void* liveWorker(void* data) {
    const auto& args = *static_cast<Worker*>(data);
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            std::ifstream input("/proc/self/task/"+std::to_string(args.leader)+"/stat");
            std::string line; std::getline(input,line); const auto end=line.rfind(')');
            if (end != std::string::npos && line.size()>end+2 && line[end+2]=='Z') break;
            CHECK(std::chrono::steady_clock::now()<deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        byte(args.notify,'Z'); CHECK(readByte(args.commands)=='E');
        _exit(0);
    } catch (...) { _exit(84); }
}
class Child {
    Fd commands, notices;
public:
    pid_t pid = -1;
    Fd accepted;
    uid_t uid = getuid() ? getuid() : 65534;
    gid_t gid = getuid() ? getgid() : 65534;
    Child(Mode mode, const char* finalizer, bool passcred=true, int type=SOCK_SEQPACKET) {
        static unsigned serial = 0;
        Fd listener(socket(AF_UNIX,type|SOCK_CLOEXEC,0)); CHECK(listener.get()>=0);
        if (passcred) { int yes=1; CHECK(!setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes))); }
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        const auto name="wddm-peer-contract-"+std::to_string(getpid())+"-"+std::to_string(++serial);
        CHECK(name.size()+1<sizeof(address.sun_path)); std::memcpy(address.sun_path+1,name.data(),name.size());
        const auto length=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+name.size());
        CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),length) && !listen(listener.get(),4));
        int go[2], notes[2]; CHECK(!pipe2(go,O_CLOEXEC) && !pipe2(notes,O_CLOEXEC));
        pid=fork(); CHECK(pid>=0);
        if (!pid) {
            ::close(go[1]); ::close(notes[0]);
            try {
                if (!getuid()) CHECK(!setgroups(0,nullptr) && !setgid(gid) && !setuid(uid));
                ::close(listener.get());
                const int connection=socket(AF_UNIX,type|SOCK_CLOEXEC,0); CHECK(connection>=0);
                CHECK(!connect(connection,reinterpret_cast<sockaddr*>(&address),length));
                const int memory=memfd_create("owned-broker-contract",MFD_CLOEXEC); CHECK(memory>=0);
                const std::uint64_t value=UINT64_C(0x793251624435feda); CHECK(write(memory,&value,sizeof(value))==sizeof(value));
                closeInherited(connection,memory,go[0],notes[1]);
                CHECK(readByte(go[0])=='S');
                if (mode==Mode::Descendant) {
                    const auto descendant=fork(); CHECK(descendant>=0);
                    if (!descendant) { sendFrame(connection,memory,Mode::Good); _exit(0); }
                    int status; CHECK(waitpid(descendant,&status,0)==descendant && WIFEXITED(status) && WEXITSTATUS(status)==0);
                } else sendFrame(connection,memory,mode);
                if (mode==Mode::Replay) sendFrame(connection,memory,Mode::Good);
                if (mode==Mode::Reset) {
                    CHECK(readByte(go[0])=='R');
                    // Closing with an unread reply produces a real Unix reset.
                    CHECK(!::close(connection)); byte(notes[1],'C');
                }
                if (mode==Mode::Finalizer) {
                    void* library=dlopen(finalizer,RTLD_NOW|RTLD_LOCAL); CHECK(library);
                    auto init=reinterpret_cast<int(*)(int,int,int,int)>(dlsym(library,"broker_finalizer_probe_init"));
                    CHECK(init && !init(connection,notes[1],go[0],memory));
                    std::exit(0); // Runs the actual ELF DSO finalizer after socket close.
                }
                if (mode==Mode::Leader) {
                    CHECK(readByte(go[0])=='L');
                    auto* args=new Worker{connection,memory,go[0],notes[1],getpid()};
                    pthread_t thread; CHECK(!pthread_create(&thread,nullptr,liveWorker,args));
                    syscall(SYS_exit,0); _exit(85); // Only leader exits; worker is still alive.
                }
                CHECK(readByte(go[0])=='E'); _exit(0);
            } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; _exit(86); }
        }
        ::close(go[0]); ::close(notes[1]); commands=Fd(go[1]); notices=Fd(notes[0]);
        pollfd ready{listener.get(),POLLIN,0}; CHECK(poll(&ready,1,5000)==1);
        accepted=Fd(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC)); CHECK(accepted.get()>=0);
    }
    ~Child() { if (pid>0) { kill(pid,SIGKILL); waitpid(pid,nullptr,0); } }
    void command(char value) { byte(commands.get(),value); }
    char notice() { return readByte(notices.get()); }
    void join() { int status; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0); pid=-1; }
};
static void valid(const Received& message) {
    CHECK(message.state==ReceiveState::Packet && message.message.payload==std::vector<std::uint8_t>(4,65));
    CHECK(message.message.fds.size()==1 && (fcntl(message.message.fds[0].get(),F_GETFD)&FD_CLOEXEC));
    std::uint64_t value=0; CHECK(pread(message.message.fds[0].get(),&value,sizeof(value),0)==sizeof(value));
    CHECK(value==UINT64_C(0x793251624435feda));
}
int main(int argc, char** argv) {
    try {
        CHECK(argc==2);
        for (const bool rawControl : {true,false}) {
            const auto before=fdCount();
            {
                Child child(Mode::Reset,argv[1]);
                Fd raw(rawControl ? fcntl(child.accepted.get(),F_DUPFD_CLOEXEC,3) : -1);
                CHECK(!rawControl || raw.get()>=0);
                Peer peer(std::move(child.accepted),child.uid,child.gid);
                child.command('S'); auto message=receive(peer);valid(message);
                peer.send(Operation::DriverCall,1,{1,2,3,4});
                child.command('R');CHECK(child.notice()=='C');
                if (rawControl) {
                    char value=0;errno=0;
                    CHECK(recv(raw.get(),&value,1,MSG_DONTWAIT)==-1 && errno==ECONNRESET);
                } else {
                    CHECK(receive(peer).state==ReceiveState::Disconnected);
                    CHECK(!peer.connected() && receive(peer).state==ReceiveState::Disconnected);
                }
                // Reset is not exit: the child is still waiting for permission
                // to finish, and its received memory remains valid.
                CHECK(!peer.processExited());valid(message);
                child.command('E');exited(peer);child.join();
            }
            CHECK(fdCount()==before);
            std::cout<<"BROKER_PEER_RESET rawErrnoControl="<<rawControl<<" processAliveAfterReset=true exitViaPidfd=true fdBalanced=true gpu=false\n";
        }
        {
            Child child(Mode::Finalizer,argv[1]); Peer peer(std::move(child.accepted),child.uid,child.gid);
            CHECK(peer.pid()==child.pid && !peer.processExited()); child.command('S'); auto message=receive(peer); valid(message);
            CHECK(child.notice()=='C'); CHECK(receive(peer).state==ReceiveState::Disconnected);
            CHECK(!peer.connected() && !peer.processExited()); child.command('F'); CHECK(child.notice()=='D');
            CHECK(!peer.processExited()); child.command('E'); exited(peer); child.join();
            std::cout<<"BROKER_PEER_FINALIZER socketEof=true aliveDuringFinalizer=true exitViaPidfd=true uid="<<child.uid<<'\n';
        }
        {
            Child child(Mode::Leader,argv[1]); Peer peer(std::move(child.accepted),child.uid,child.gid);
            child.command('S'); valid(receive(peer)); child.command('L'); CHECK(child.notice()=='Z');
            CHECK(!peer.processExited()); child.command('E'); exited(peer); child.join();
            std::cout<<"BROKER_PEER_THREAD_GROUP leaderZombie=true liveWorkerRetained=true\n";
        }
        for (auto mode : {Mode::Magic,Mode::Version,Mode::Reserved,Mode::Length,Mode::FdCount,Mode::Sequence,
                          Mode::Operation,Mode::Oversize,Mode::ExcessFds,Mode::Empty,Mode::Replay,Mode::Descendant,Mode::Status}) {
            Child child(mode,argv[1]); Peer peer(std::move(child.accepted),child.uid,child.gid); child.command('S');
            if (mode==Mode::Replay) valid(receive(peer));
            const auto before=fdCount(); bool rejected=false;
            try { auto result=receive(peer); rejected=mode==Mode::Empty && result.state==ReceiveState::Disconnected; }
            catch (const std::system_error& error) { rejected=error.code().value()==(mode==Mode::Descendant ? EPERM : EPROTO); }
            CHECK(rejected && !peer.connected() && !peer.processExited()); CHECK(fdCount()==before-1);
            child.command('E'); exited(peer); child.join();
        }
        {
            Child child(Mode::Good,argv[1]); bool denied=false;
            try { Peer peer(std::move(child.accepted),child.uid+1,child.gid); }
            catch (const std::system_error& error) { denied=error.code().value()==EPERM; }
            CHECK(denied);
        }
        for (bool credentials : {false,true}) {
            Child child(Mode::Good,argv[1],credentials,credentials ? SOCK_STREAM : SOCK_SEQPACKET); bool denied=false;
            try { Peer peer(std::move(child.accepted),child.uid,child.gid); }
            catch (const std::system_error& error) { denied=error.code().value()==EPROTO; }
            CHECK(denied);
        }
        {
            // An unexpected descriptor-bearing ancillary feature must neither
            // authenticate the wrong identity nor leak its installed pidfd.
            Child child(Mode::Good,argv[1]);
            Fd other(fcntl(child.accepted.get(),F_DUPFD_CLOEXEC,3)); CHECK(other.get()>=0);
            Peer peer(std::move(child.accepted),child.uid,child.gid);
            int yes=1; CHECK(!setsockopt(other.get(),SOL_SOCKET,SO_PASSPIDFD,&yes,sizeof(yes)));
            const auto before=fdCount(); child.command('S'); bool denied=false;
            try { receive(peer); } catch (const std::system_error& error) { denied=error.code().value()==EPROTO; }
            CHECK(denied && fdCount()==before-1 && !peer.processExited()); child.command('E'); exited(peer); child.join();
        }
        {
            Child first(Mode::Good,argv[1]), second(Mode::Good,argv[1]);
            Peer a(std::move(first.accepted),first.uid,first.gid), b(std::move(second.accepted),second.uid,second.gid);
            CHECK(a.pid()!=b.pid()); first.command('S'); second.command('S'); valid(receive(a)); valid(receive(b));
            first.command('E'); exited(a); first.join(); CHECK(!b.processExited()); second.command('E'); exited(b); second.join();
        }
        std::cout<<"BROKER_PEER_CONTRACT_PASS malformedCases=14 fdLeaks=0 independentPeers=true gpu=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"BROKER_PEER_CONTRACT_FAIL "<<error.what()<<" errno="<<errno<<'\n'; return 1; }
}
