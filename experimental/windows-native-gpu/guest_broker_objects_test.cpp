// SPDX-License-Identifier: MIT
// Native GPU operations and kernel object ioctls are mocks. Unix credentials,
// SCM_RIGHTS, pinned producer identity and last socket-FD close are real.
#include "guest_broker_server.h"
#include <iostream>
#include <grp.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
using namespace guest_broker;
using namespace driver_bridge;
static unsigned checks=0;
#define CHECK(c) do { ++checks; if (!(c)) throw std::runtime_error("Check failed: " #c); } while (0)
template<class F> static void denied(F f,int code) {
    bool rejected=false;
    try { f(); } catch (const std::system_error& e) { rejected=e.code().value()==code; }
    CHECK(rejected);
}
static std::vector<std::uint8_t> packet(Op op,std::uint32_t id=0,const std::vector<std::uint8_t>& data={}) {
    auto bytes=encode(Header{static_cast<std::uint32_t>(op),id,0,0}); bytes.insert(bytes.end(),data.begin(),data.end()); return bytes;
}
static std::vector<std::uint8_t> receipt(Op op,std::uint32_t id,const std::vector<std::uint8_t>& data={}) {
    auto bytes=packet(op,id,encode(Reply{})); bytes.insert(bytes.end(),data.begin(),data.end()); return bytes;
}
struct ObjectMock {
    Fd nativeBuffer(int,std::uint64_t) { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No native DMA-BUF in mock"); }
    std::uint64_t nativeBufferInfo(int) { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No native DMA-BUF in mock"); }
    wddm_native_buffer_stats nativeBufferStats() { return {WDDM_APERTURE_VERSION,0,0,0,0}; }
    wddm_native_buffer_caps nativeBufferCaps() { return {WDDM_APERTURE_VERSION,0,{0,0}}; }
    std::pair<Fd,std::uint64_t> resolveSyncFile(int) { throw std::system_error(EINVAL,std::generic_category(),"No real sync-file in mock"); }
    struct Record { Fd watch; ObjectRef ref; std::uint64_t identity; };
    std::map<std::uint32_t,Record> records;
    bool corrupt=false;
    unsigned resolveRefusals=0;
    int resolveError=ENOMEM;
    unsigned reapCount=0;
    static std::uint64_t identity(int fd) {
        std::uint64_t cookie=0; socklen_t bytes=sizeof(cookie);
        if (getsockopt(fd,SOL_SOCKET,SO_COOKIE,&cookie,&bytes) || bytes!=sizeof(cookie))
            throw std::system_error(EBADF,std::generic_category(),"Mock foreign FD");
        return cookie;
    }
    wddm_object_stats info() { wddm_object_stats value{}; value.version=WDDM_APERTURE_VERSION; return value; }
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        int sockets[2]; CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
        Fd recipient(sockets[0]),watch(sockets[1]); const ObjectRef ref{kind,ticket,ticket+100ull};
        records.emplace(ticket,Record{std::move(watch),ref,identity(recipient.get())});
        return {std::move(recipient),ref};
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        const auto cookie=identity(fd);
        for (const auto& item:records) if (item.second.identity==cookie) {
            if (item.second.ref.kind!=kind) throw std::system_error(EINVAL,std::generic_category(),"Mock wrong kind");
            if (resolveRefusals) { --resolveRefusals; throw std::system_error(resolveError,std::generic_category(),"Mock native alias refusal"); }
            auto ref=item.second.ref; if (corrupt) ++ref.cookie; return ref;
        }
        throw std::system_error(EBADF,std::generic_category(),"Mock unregistered FD");
    }
    ObjectRef reap() {
        for (auto i=records.begin();i!=records.end();++i) {
            pollfd event{i->second.watch.get(),POLLIN,0}; CHECK(poll(&event,1,0)>=0);
            if (event.revents&POLLHUP) { const auto ref=i->second.ref; records.erase(i); ++reapCount; return ref; }
        }
        return {};
    }
};
static void objectPolicy() {
    ObjectStore store; ObjectMock kernel;
    denied([&]{store.create({WDDM_OBJECT_RESOURCE,11,0},kernel);},EBADF);
    const auto exportPacket=packet(Op::ExportResource,9);
    store.observe(exportPacket,receipt(Op::ExportResource,11)); CHECK(store.size()==1);
    auto cap=store.create({WDDM_OBJECT_RESOURCE,11,0},kernel);
    CHECK(cap.second.ticket==11 && cap.second.cookie==111 && (fcntl(cap.first.get(),F_GETFD)&FD_CLOEXEC));
    denied([&]{store.create({WDDM_OBJECT_RESOURCE,11,0},kernel);},EPROTO);
    denied([&]{store.observe(exportPacket,receipt(Op::ExportResource,11));},EPROTO);
    denied([&]{store.authorize(packet(Op::CloseSharedResource,11),{},kernel);},EPERM);
    denied([&]{store.resolve(cap.first.get(),WDDM_OBJECT_SYNC,kernel);},EINVAL);
    Fd foreign(open("/dev/null",O_RDONLY|O_CLOEXEC));
    denied([&]{store.resolve(foreign.get(),WDDM_OBJECT_RESOURCE,kernel);},EBADF);
    kernel.corrupt=true; denied([&]{store.resolve(cap.first.get(),WDDM_OBJECT_RESOURCE,kernel);},EPROTO); kernel.corrupt=false;
    const auto query=packet(Op::QuerySharedResourceInfo,11,encode(SharedResourceQueryDesc{2,0}));
    denied([&]{store.authorize(query,{},kernel);},EPERM);
    std::vector<Fd> passed; passed.emplace_back(fcntl(cap.first.get(),F_DUPFD_CLOEXEC,0));
    store.authorize(query,passed,kernel); CHECK(store.resolve(passed[0].get(),WDDM_OBJECT_RESOURCE,kernel).ticket==11);
    denied([&]{store.authorize(packet(Op::ImportResource,12),passed,kernel);},EPERM);
    denied([&]{store.authorize(packet(Op::Hello),passed,kernel);},EPROTO);
    cap.first=Fd(); CHECK(!store.reap(kernel).ticket); // transferred recipient still owns it
    passed.clear(); CHECK(store.reap(kernel).ticket==11 && kernel.reapCount==1);
    CHECK(!store.reap(kernel).ticket);
    store.authorize(packet(Op::CloseSharedResource,11),{},kernel);
    store.observe(packet(Op::CloseSharedResource,11),receipt(Op::CloseSharedResource,11)); CHECK(!store.size());
    denied([&]{store.authorize(packet(Op::CloseSharedResource,11),{},kernel);},EBADF);
    // Publication failure can close an unpublished export, never a live FD.
    store.observe(packet(Op::ExportSync,3),receipt(Op::ExportSync,12));
    store.authorize(packet(Op::CloseSharedSync,12),{},kernel);
    store.observe(packet(Op::CloseSharedSync,12),receipt(Op::CloseSharedSync,12)); CHECK(!store.size());
    store.observe(packet(Op::ExportSync,3),receipt(Op::ExportSync,13));
    auto fence=store.create({WDDM_OBJECT_SYNC,13,0},kernel);
    store.syncLineage(fence.first.get(),3,kernel);
    denied([&]{store.syncLineage(fence.first.get(),4,kernel);},EPERM);
    denied([&]{store.syncLineage(fence.first.get(),0,kernel);},EPERM);
    denied([&]{store.syncLineage(foreign.get(),3,kernel);},EBADF);
    fence.first=Fd(); CHECK(store.reap(kernel).ticket==13);
    store.authorize(packet(Op::CloseSharedSync,13),{},kernel);
    store.observe(packet(Op::CloseSharedSync,13),receipt(Op::CloseSharedSync,13)); CHECK(!store.size());
    for (unsigned i=1;i<=16;++i) store.observe(exportPacket,receipt(Op::ExportResource,20+i));
    denied([&]{store.observe(exportPacket,receipt(Op::ExportResource,37));},EPROTO);
    CHECK(store.size()==16);
    std::cout<<"BROKER_OBJECT_POLICY checks="<<checks<<" native=mock kernel=mock gpu=false\n";
}
static void importedViews() {
    Views views; views.observe(packet(Op::OpenAdapter),receipt(Op::OpenAdapter,1));
    views.observe(packet(Op::CreateDevice,1),receipt(Op::CreateDevice,2));
    const auto desc=SharedResourceOpenDesc{2,4,8,0};
    auto data=encode(SharedResourceOpenReply{4,8,0,0}); data.resize(data.size()+12);
    views.observe(packet(Op::ImportResource,11,encode(desc)),receipt(Op::ImportResource,3,data));
    auto lock=receipt(Op::LockVendorAllocation,3,encode(VendorCpuReply{4096,0,77}));
    Reply result{0,0,1048576}; std::memcpy(lock.data()+sizeof(Header),&result,sizeof(result));
    views.observe(packet(Op::LockVendorAllocation,3,encode(VendorCpuDesc{2,0})),lock);
    views.acquire({WDDM_APERTURE_ALLOCATION,0,1048576,4096,77}).cookie=19;
    denied([&]{views.approve(packet(Op::DestroyVendorResource,4,encode(std::uint32_t{2})));},EBUSY);
    const auto sync=SharedSyncOpenDesc{2,NoGpuAccessSyncFlag,0,0};
    views.observe(packet(Op::ImportSync,12,encode(sync)),receipt(Op::ImportSync,5,encode(SyncReply{4104,0})));
    CHECK(views.acquire({WDDM_APERTURE_FENCE,0,4096,4096,0}).cookie==0);
    // Exercise the actual broker receipt policy in both compiled profiles.
    Views large; large.observe(packet(Op::OpenAdapter),receipt(Op::OpenAdapter,1));
    large.observe(packet(Op::CreateDevice,1),receipt(Op::CreateDevice,2));
    large.observe(packet(Op::CreateVendorAllocation,2),receipt(Op::CreateVendorAllocation,3));
    auto full=receipt(Op::LockVendorAllocation,3,encode(VendorCpuReply{33226752,0,78}));
    std::memcpy(full.data()+sizeof(Header),&result,sizeof(result));
    const auto lockRequest=packet(Op::LockVendorAllocation,3,encode(VendorCpuDesc{2,0}));
    if (gpu_diagnostic::HighResolution) {
        large.observe(lockRequest,full);
        CHECK(large.acquire({WDDM_APERTURE_ALLOCATION,0,1048576,33226752,78}).cookie==0);
    } else denied([&]{large.observe(lockRequest,full);},EPROTO);
    std::cout<<"BROKER_IMPORTED_VIEWS highResolution="<<gpu_diagnostic::HighResolution<<" native=mock kernel=mock gpu=false\n";
}
struct MemoryMock {
    Fd nativeSyncFile(SyncFileRequest,int) { throw std::runtime_error("Unexpected linked sync-file export"); }
    Fd syncFile(SyncFileRequest) { throw std::runtime_error("Unexpected sync-file export"); }
    wddm_sync_file_stats syncFileInfo() { throw std::runtime_error("Unexpected sync-file stats"); }
    bool closed=false;
    wddm_aperture_desktop_slots desktopSlotCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No desktop slot kernel in portable test"); }
    wddm_aperture_desktop_fences desktopFenceCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No desktop fence kernel in portable test"); }
    wddm_aperture_coordinator_caps coordinatedCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No coordinator kernel in portable test"); }
    wddm_aperture_caps capacity() { return {1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}}; }
    wddm_aperture_info info() { return {1,128,256,0,128*4096,256*1048576,1048576,0,0,0,0}; }
    std::pair<Fd,std::uint64_t> acquire(ViewRequest) { throw std::runtime_error("Unexpected memory request"); }
    void release(std::uint64_t) { throw std::runtime_error("Unexpected memory release"); }
    void close() { closed=true; }
};
struct NativeMock {
    MemoryMock& memory; ObjectMock& objects; unsigned queries=0,closes=0;
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& bytes) {
        Header h{}; std::memcpy(&h,bytes.data(),sizeof(h)); const auto op=static_cast<Op>(h.type);
        if (op==Op::ExportResource && h.handle==9) return receipt(op,11);
        if (op==Op::QuerySharedResourceInfo && h.handle==11) { ++queries; return receipt(op,11); }
        if (op==Op::CloseSharedResource && h.handle==11) {
            CHECK(memory.closed && objects.reapCount==1 && objects.records.empty()); ++closes; return receipt(op,11);
        }
        throw std::runtime_error("Unexpected mock native call");
    }
};
static void recipientAfterProducer() {
    char directory[]="/tmp/wddm-broker-objects-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chmod(directory,0755));
    const auto path=std::string(directory)+"/broker.sock";
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0)); const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled)));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    const auto child=fork(); CHECK(child>=0);
    if (!child) {
        try {
            close(listener.get()); CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            setenv("WDDM_BRIDGE_BROKER_SOCKET",path.c_str(),1); Client client;
            CHECK(client.objectInfo().version==WDDM_APERTURE_VERSION);
            client.driver(packet(Op::ExportResource,9)); auto cap=client.createObject(11,WDDM_OBJECT_RESOURCE);
            denied([&]{client.resolveObject(cap.fds[0].get(),WDDM_OBJECT_SYNC);},EINVAL);
            Fd foreign(open("/dev/null",O_RDONLY|O_CLOEXEC));
            denied([&]{client.resolveObject(foreign.get(),WDDM_OBJECT_RESOURCE);},EBADF);
            CHECK(client.resolveObject(cap.fds[0].get(),WDDM_OBJECT_RESOURCE).ticket==11);
            denied([&]{client.nativeBuffer(cap.fds[0].get(),4096);},EOPNOTSUPP);
            denied([&]{client.nativeBuffer(foreign.get(),4096);},EBADF);
            denied([&]{client.nativeBuffer(cap.fds[0].get(),0);},EINVAL);
            denied([&]{client.nativeBuffer(cap.fds[0].get(),WDDM_NATIVE_BUFFER_MAX_BYTES+1);},EINVAL);
            denied([&]{client.nativeBufferInfo(cap.fds[0].get());},EOPNOTSUPP);
            const auto buffers=client.nativeBufferStats();
            const auto bufferCaps=client.nativeBufferCaps(); CHECK(bufferCaps.version==WDDM_APERTURE_VERSION && !bufferCaps.flags);
            CHECK(!buffers.held && !buffers.bytes && !buffers.created && !buffers.released);
            CHECK(fcntl(cap.fds[0].get(),F_GETFD)>=0); // failed SCM_RIGHTS requests preserve caller ownership
            client.driver(packet(Op::QuerySharedResourceInfo,11,encode(SharedResourceQueryDesc{2,0})));
            const auto recipient=fork(); CHECK(recipient>=0);
            if (!recipient) { poll(nullptr,0,400); _exit(0); }
            // The descendant inherits both the capability and socket. It
            // sends no GPU work; producer pidfd exit must still start drain.
            _exit(0);
        } catch (const std::exception& error) { dprintf(2,"Object RPC child: %s\n",error.what()); _exit(1); }
    }
    Fd accepted(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK)); CHECK(accepted.get()>=0);
    Peer peer(std::move(accepted),65534,65534); MemoryMock memory; ObjectMock objects; NativeMock native{memory,objects};
    const auto start=std::chrono::steady_clock::now();
    CHECK(!serve(peer,native,memory,objects));
    CHECK(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(300));
    int status=0; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(peer.processExited() && memory.closed && native.queries==1 && native.closes==1 && objects.reapCount==1);
    CHECK(!unlink(path.c_str()) && !rmdir(directory));
    std::cout<<"BROKER_OBJECT_RPC clientUid=65534 inheritedSocket=true recipientAfterProducer=true memoryRevokedFirst=true nativeCloses=1 native=mock kernel=mock gpu=false\n";
}
static void resolutionRefusal(int error,bool recoverable,bool malformed=false) {
    char directory[]="/tmp/wddm-broker-refusal-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chmod(directory,0755));
    const auto path=std::string(directory)+"/broker.sock";
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0)); const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    const auto child=fork(); CHECK(child>=0);
    if (!child) {
        try {
            close(listener.get()); CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            setenv("WDDM_BRIDGE_BROKER_SOCKET",path.c_str(),1); Client client;
            client.driver(packet(Op::ExportResource,9)); auto cap=client.createObject(11,WDDM_OBJECT_RESOURCE);
            if (malformed) denied([&]{client.request(Operation::ResolveObject,encode(ObjectRequest{WDDM_OBJECT_RESOURCE,0,0}));},EPROTO);
            else denied([&]{client.resolveObject(cap.fds[0].get(),WDDM_OBJECT_RESOURCE);},error);
            CHECK(fcntl(cap.fds[0].get(),F_GETFD)>=0); // Refusal cannot revoke the sender's live capability.
            if (recoverable) {
                CHECK(client.objectInfo().version==WDDM_APERTURE_VERSION);
                CHECK(client.resolveObject(cap.fds[0].get(),WDDM_OBJECT_RESOURCE).ticket==11);
                client.driver(packet(Op::QuerySharedResourceInfo,11,encode(SharedResourceQueryDesc{2,0})));
            } else denied([&]{client.objectInfo();},ESHUTDOWN);
            _exit(0);
        } catch (const std::exception& e) { dprintf(2,"Resolution refusal child: %s\n",e.what()); _exit(1); }
    }
    Fd accepted(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK)); CHECK(accepted.get()>=0);
    Peer peer(std::move(accepted),65534,65534); MemoryMock memory; ObjectMock objects;
    objects.resolveRefusals=1;objects.resolveError=error;NativeMock native{memory,objects};
    CHECK(serve(peer,native,memory,objects)==(recoverable ? 0 : 1));
    int status=0; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(peer.processExited() && memory.closed && objects.records.empty() && objects.reapCount==1 && native.closes==1 &&
          native.queries==(recoverable ? 1u : 0u) && objects.resolveRefusals==(malformed ? 1u : 0u));
    CHECK(!unlink(path.c_str()) && !rmdir(directory));
    const auto marker="BROKER_RESOLVE_REFUSAL errno="+std::to_string(error)+" recoverable="+std::to_string(recoverable)+
        " malformed="+std::to_string(malformed)+" sameConnectionRetry="+std::to_string(recoverable)+
        " liveFdPreserved=true pidfdExitRequired=true native=mock kernel=mock gpu=false\n";
    std::cout.flush();
    CHECK(write(STDOUT_FILENO,marker.data(),marker.size())==static_cast<ssize_t>(marker.size()));
}
int main() {
    try {
        alarm(15); objectPolicy(); importedViews();
        if (geteuid()) std::cout<<"BROKER_OBJECT_RPC skipped=root-required\n";
        else {
            recipientAfterProducer(); resolutionRefusal(ENOMEM,true); resolutionRefusal(ENOSPC,true);
            resolutionRefusal(EIO,false); resolutionRefusal(ETIMEDOUT,false); resolutionRefusal(EPROTO,false,true);
        }
        alarm(0); return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
