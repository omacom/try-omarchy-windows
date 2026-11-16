// SPDX-License-Identifier: MIT
// Actual guest admission loop, Unix credentials and pidfds; GPU callback mocked.
#include "guest_broker_session.h"
#include <grp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <iostream>

using namespace guest_broker;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(0)
static void put(int fd,const void* data,std::size_t bytes) { CHECK(write(fd,data,bytes)==static_cast<ssize_t>(bytes)); }
static void get(int fd,void* data,std::size_t bytes) {
    auto* out=static_cast<char*>(data);
    while(bytes) { pollfd ready{fd,POLLIN,0};CHECK(poll(&ready,1,5000)==1);const auto n=read(fd,out,bytes);CHECK(n>0);out+=n;bytes-=static_cast<std::size_t>(n); }
}
struct Child {
    pid_t pid=-1;
    int wait() { int status=0;CHECK(pid>0 && waitpid(pid,&status,0)==pid);pid=-1;CHECK(WIFEXITED(status));return WEXITSTATUS(status); }
    ~Child() { if(pid>0) { kill(pid,SIGKILL);waitpid(pid,nullptr,0); } }
};
struct Directory {
    std::string folder,path;
    Directory() { char value[]="/tmp/omarchy-session-XXXXXX";CHECK(mkdtemp(value));folder=value;CHECK(!chmod(folder.c_str(),0711));path=folder+"/gpu.sock"; }
    ~Directory() { unlink(path.c_str());rmdir(folder.c_str()); }
};
static int client(const std::string& path,int start,int ack,int release,bool denialAllowed) {
    alarm(12);
    if(setgroups(0,nullptr) || setgid(65534) || setuid(65534)) return 10;
    char value=0;get(start,&value,1);CHECK(value=='S');
    sockaddr_un address{};address.sun_family=AF_UNIX;CHECK(path.size()<sizeof address.sun_path);std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd channel;const auto deadline=SessionPolicy::Clock::now()+std::chrono::seconds(3);
    for(;;) {
        channel=Fd(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0));CHECK(channel.get()>=0);
        const int enabled=1;CHECK(!setsockopt(channel.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled));
        if(!connect(channel.get(),reinterpret_cast<sockaddr*>(&address),sizeof address)) break;
        CHECK(errno==ENOENT || errno==ECONNREFUSED || errno==EAGAIN || errno==EINPROGRESS);
        CHECK(SessionPolicy::Clock::now()<deadline);poll(nullptr,0,2);
    }
    try {
        Peer peer(std::move(channel),0,0,true);peer.send(Operation::Hello,1,{42});
        for(;;) {
            const auto packet=peer.receive();
            if(packet.state==ReceiveState::Idle) { CHECK(SessionPolicy::Clock::now()<deadline);peer.waitReadable(10);continue; }
            if(packet.state==ReceiveState::Disconnected) return denialAllowed?2:11;
            CHECK(packet.message.frame.operation==Operation::Hello && packet.message.payload==std::vector<std::uint8_t>({42}));break;
        }
        if(ack>=0) put(ack,"R",1);
        if(release>=0) { get(release,&value,1);CHECK(value=='F'); }
    } catch(const std::system_error& error) {
        if(denialAllowed && (error.code().value()==EPIPE || error.code().value()==ECONNRESET || error.code().value()==ENOTCONN)) return 2;
        throw;
    }
    return 0;
}
struct Client:Child {
    Fd start,ack,release;
    Client(const std::string& path,bool held,bool denialAllowed=false,int sharedAck=-1,int sharedRelease=-1) {
        int begin[2],ready[2],finish[2];CHECK(!pipe2(begin,O_CLOEXEC));CHECK(!pipe2(ready,O_CLOEXEC));CHECK(!pipe2(finish,O_CLOEXEC));
        pid=fork();CHECK(pid>=0);
        if(!pid) {
            close(begin[1]);close(ready[0]);close(finish[1]);
            try { _exit(client(path,begin[0],sharedAck>=0?sharedAck:(held?ready[1]:-1),sharedRelease>=0?sharedRelease:(held?finish[0]:-1),denialAllowed)); }
            catch(const std::exception& error) { std::fprintf(stderr,"CLIENT_CONTROL_FAILURE %s\n",error.what());_exit(13); }
        }
        close(begin[0]);close(ready[1]);close(finish[0]);start=Fd(begin[1]);ack=Fd(ready[0]);release=Fd(finish[1]);
    }
    void begin() { put(start.get(),"S",1);start=Fd(); }
    void ready() { char value=0;get(ack.get(),&value,1);CHECK(value=='R'); }
    void finish() { put(release.get(),"F",1);release=Fd(); }
};
struct Report { std::uint64_t connections,failures;std::uint32_t peak,complete; };
struct Broker:Child {
    Fd ready,result;
    Broker(const std::string& path,pid_t primary,bool session,unsigned actorLimit=4) {
        int pair[2],started[2],done[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));CHECK(!pipe2(started,O_CLOEXEC));CHECK(!pipe2(done,O_CLOEXEC));
        Fd primaryProcess(static_cast<int>(syscall(SYS_pidfd_open,primary,0)));CHECK(primaryProcess.get()>=0);
        pid=fork();CHECK(pid>=0);
        if(!pid) {
            alarm(20);close(pair[0]);close(started[0]);close(done[0]);
            try {
                SessionPolicy policy(session,pair[1]);close(pair[1]);
                Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));CHECK(listener.get()>=0);
                const int credentials=1;CHECK(!setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&credentials,sizeof credentials));
                sockaddr_un address{};address.sun_family=AF_UNIX;CHECK(path.size()<sizeof address.sun_path);std::memcpy(address.sun_path,path.c_str(),path.size()+1);
                CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),4));
                put(started[1],"B",1);close(started[1]);
                auto outcome=runPeerGroup(listener,65534,65534,static_cast<std::uint64_t>(primary),primaryProcess.get(),actorLimit,policy,[](Peer& peer,bool) {
                    while(peer.connected() && !peer.processExited()) {
                        auto packet=peer.receive();if(packet.state==ReceiveState::Disconnected) break;
                        if(packet.state==ReceiveState::Idle) { peer.waitReadable(5);continue; }
                        CHECK(packet.message.frame.operation==Operation::Hello && packet.message.payload==std::vector<std::uint8_t>({42}));
                        peer.send(Operation::Hello,packet.message.frame.sequence,packet.message.payload);
                    }
                    return 0;
                });
                const Report report{outcome.connections,outcome.failures,outcome.peakWorkers,static_cast<std::uint32_t>(outcome.primaryAccepted && outcome.primaryFinished && outcome.connectionCountExact && listener.get()<0)};
                put(done[1],&report,sizeof report);_exit(0);
            } catch(const std::exception& error) { std::fprintf(stderr,"BROKER_CONTROL_FAILURE %s\n",error.what());_exit(14); }
        }
        close(pair[0]);close(pair[1]);close(started[1]);close(done[1]);ready=Fd(started[0]);result=Fd(done[0]);char value=0;get(ready.get(),&value,1);CHECK(value=='B');
    }
    Report finish() { Report report{};get(result.get(),&report,sizeof report);CHECK(wait()==0);return report; }
};
static void rolling(bool session,unsigned actorLimit=4) {
    Directory folder;Client primary(folder.path,true);Broker broker(folder.path,primary.pid,session,actorLimit);
    // An ordinary same-UID process still cannot take the primary role.
    { Client foreign(folder.path,false,true);foreign.begin();CHECK(foreign.wait()==2); }
    primary.begin();primary.ready();
    unsigned accepted=0,denied=0;
    for(unsigned i=0;i<128;++i) { Client actor(folder.path,false,!session);actor.begin();const int result=actor.wait();if(result==0) ++accepted;else { CHECK(result==2);++denied; } }
    if(session) {
        CHECK(accepted==128 && denied==0);
        int ack[2],release[2];CHECK(!pipe2(ack,O_CLOEXEC));CHECK(!pipe2(release,O_CLOEXEC));Fd readAck(ack[0]),writeAck(ack[1]),readRelease(release[0]),writeRelease(release[1]);
        std::vector<std::unique_ptr<Client>> clients;
        for(unsigned i=0;i<16;++i) { auto actor=std::make_unique<Client>(folder.path,false,false,writeAck.get(),readRelease.get());actor->begin();clients.push_back(std::move(actor)); }
        std::vector<char> first(actorLimit-1);get(readAck.get(),first.data(),first.size());CHECK(std::string(first.data(),first.size())==std::string(actorLimit-1,'R'));
        pollfd extra{readAck.get(),POLLIN,0};CHECK(poll(&extra,1,150)==0); // primary consumes one negotiated actor slot.
        const std::string finish(16,'F');put(writeRelease.get(),finish.data(),finish.size());
        for(auto& actor:clients) CHECK(actor->wait()==0);
        accepted+=16;
    } else CHECK(accepted==63 && denied==65);
    primary.finish();CHECK(primary.wait()==0);const auto report=broker.finish();
    CHECK(report.complete && !report.failures && report.connections==accepted+1 && report.peak<=actorLimit);
    if(session) CHECK(report.connections==145 && report.peak==actorLimit);
    std::cout<<"SESSION_ADMISSION_CONTROL enabled="<<session<<" accepted="<<report.connections<<" denied="<<denied<<" peakWorkers="<<report.peak<<" realCredentials=true mockGpu=true\n";
}
static void policyControls() {
    CHECK(!SessionPolicy::requested(nullptr) && SessionPolicy::requested("1"));
    for(const bool enabled:{false,true}) {
        CHECK(SessionPolicy::retirementWitnesses(nullptr,enabled) && SessionPolicy::retirementWitnesses("1",enabled));
        for(const char* value:{"","true","false","01","0\n"}) {
            bool failed=false;try { (void)SessionPolicy::retirementWitnesses(value,enabled); } catch(const std::exception&) { failed=true; }CHECK(failed);
        }
    }
    CHECK(!SessionPolicy::retirementWitnesses("0",true));
    bool badWitness=false;try { (void)SessionPolicy::retirementWitnesses("0",false); } catch(const std::exception&) { badWitness=true; }CHECK(badWitness);
    for(const char* value:{"","0","true","01","1\n"}) { bool failed=false;try { SessionPolicy::requested(value); } catch(const std::exception&) { failed=true; }CHECK(failed); }
    SessionPolicy legacy(false,-1);CHECK(legacy.mayAccept(63) && !legacy.mayAccept(64));
    for(int fd:{-1,STDIN_FILENO}) { bool failed=false;try { SessionPolicy policy(true,fd); } catch(const std::exception&) { failed=true; }CHECK(failed); }
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));Fd first(pair[0]),second(pair[1]);
    bool failed=false;try { SessionPolicy policy(true,first.get()); } catch(const std::exception&) { failed=true; }CHECK(failed); // self is not the supervising parent.
    Child child;child.pid=fork();CHECK(child.pid>=0);
    if(!child.pid) {
        alarm(5);
        try {
            const auto start=SessionPolicy::Clock::now();SessionPolicy policy(true,first.get(),start);
            CHECK(policy.mayAccept(UINT64_MAX));
            CHECK(!policy.failure(false,false,0,start+std::chrono::seconds(29)));
            CHECK(std::string(policy.failure(false,false,0,start+std::chrono::seconds(30)))=="primary-connection-deadline");
            CHECK(!policy.failure(true,false,4,start+std::chrono::hours(24)));
            const auto drain=start+std::chrono::hours(24);CHECK(!policy.failure(true,true,1,drain));
            CHECK(!policy.failure(true,true,1,drain+std::chrono::seconds(9)));
            CHECK(std::string(policy.failure(true,true,1,drain+std::chrono::seconds(10)))=="secondary-drain-deadline");
            _exit(0);
        } catch(...) { _exit(15); }
    }
    CHECK(child.wait()==0);
}
static void ownerExit() {
    const int inheritedFlags=fcntl(STDERR_FILENO,F_GETFL);CHECK(inheritedFlags>=0);
    Directory folder;int identities[2];CHECK(!pipe2(identities,O_CLOEXEC));Fd output(identities[1]),input(identities[0]);
    int keepAlive[2];CHECK(!pipe2(keepAlive,O_CLOEXEC));Fd keepRead(keepAlive[0]),keepWrite(keepAlive[1]);
    Child supervisor;supervisor.pid=fork();CHECK(supervisor.pid>=0);
    if(!supervisor.pid) {
        alarm(10);
        try {
            Client primary(folder.path,true,false,-1,keepRead.get());Broker broker(folder.path,primary.pid,true);primary.begin();primary.ready();
            pid_t owned[2]{broker.pid,primary.pid};put(output.get(),owned,sizeof owned);
            _exit(0); // kernel marks the broker's held supervisor pidfd exited.
        } catch(...) { _exit(16); }
    }
    pid_t owned[2];get(input.get(),owned,sizeof owned);CHECK(supervisor.wait()==0);
    Child broker;broker.pid=owned[0];Child primary;primary.pid=owned[1];CHECK(broker.wait()==125);
    int status=0;CHECK(waitpid(primary.pid,&status,WNOHANG)==0);CHECK(!kill(primary.pid,SIGKILL));
    CHECK(waitpid(primary.pid,&status,0)==primary.pid && WIFSIGNALED(status));primary.pid=-1;
    CHECK(fcntl(STDERR_FILENO,F_GETFL)==inheritedFlags);
    std::cout<<"SESSION_OWNER_EXIT_CONTROL pinnedRoot=true containmentExit=125 inheritedDiagnosticFlagsUnchanged=true mockGpu=true\n";
}
int main() {
    try {
        CHECK(!getuid() && !geteuid());alarm(55);CHECK(!prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0));
        policyControls();rolling(false);rolling(true,4);rolling(true,8);ownerExit();
        std::cout<<"GUEST_BROKER_SESSION_CONTROLS_PASS realUnixPeers=true realPidfds=true rollingConnections=145 maxWorkers=8 gpu=false vm=false physicalInput=false\n";return 0;
    } catch(const std::exception& error) { std::cerr<<"GUEST_BROKER_SESSION_CONTROL_FAILED "<<error.what()<<'\n';return 1; }
}
