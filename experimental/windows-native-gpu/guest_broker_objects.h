// SPDX-License-Identifier: MIT
// Retained native NT references backed by kernel-authenticated capability FDs.
#pragma once
#include "guest_broker_client.h"
#include <map>
#include <cstdio>
#include <sys/syscall.h>

namespace guest_broker {
class KernelObjects {
    Fd control;
    void open() {
        if (control.get()>=0) return;
        control=Fd(static_cast<int>(syscall(SYS_openat,AT_FDCWD,"/dev/wddm-aperture",O_RDWR|O_CLOEXEC,0)));
        if (control.get()<0) throw std::system_error(errno,std::generic_category(),"Broker object controller open");
    }
    void call(unsigned long request,void* argument) {
        open();
        if (syscall(SYS_ioctl,control.get(),request,argument))
            throw std::system_error(errno,std::generic_category(),"Broker kernel object operation");
    }
public:
    wddm_object_stats info() {
        wddm_object_stats value{}; value.version=WDDM_APERTURE_VERSION; call(WDDM_OBJECT_STATS,&value);
        if (value.version!=WDDM_APERTURE_VERSION || value.reserved || value.held>WDDM_OBJECT_MAX || value.active>value.held)
            throw std::system_error(EPROTO,std::generic_category(),"Broker object stats mismatch");
        return value;
    }
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        wddm_object_create value{}; value.version=WDDM_APERTURE_VERSION; value.kind=kind; value.ticket=ticket;
        call(WDDM_OBJECT_CREATE,&value); Fd descriptor(value.fd);
        if (descriptor.get()<0 || !value.cookie || value.ticket!=ticket || value.kind!=kind || value.reserved[0] || value.reserved[1])
            throw std::system_error(EPROTO,std::generic_category(),"Broker object publication mismatch");
        return {std::move(descriptor),ObjectRef{kind,ticket,value.cookie}};
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        wddm_object_resolve value{}; value.version=WDDM_APERTURE_VERSION; value.kind=kind; value.fd=fd;
        call(WDDM_OBJECT_RESOLVE,&value);
        if (!value.ticket || !value.cookie || value.reserved || value.kind!=kind)
            throw std::system_error(EPROTO,std::generic_category(),"Broker object resolution mismatch");
        return {value.kind,value.ticket,value.cookie};
    }
    std::pair<Fd,std::uint64_t> resolveSyncFile(int fd) {
        wddm_sync_file_native_resolve value{}; value.version=WDDM_APERTURE_VERSION; value.fd=fd;
        call(WDDM_SYNC_FILE_NATIVE_RESOLVE,&value);
        if(value.native_fd<0 || value.value==UINT64_MAX || value.reserved || value.flags)
            throw std::system_error(EPROTO,std::generic_category(),"Kernel sync-file lineage mismatch");
        return {Fd(value.native_fd),value.value};
    }
    Fd nativeBuffer(int fd,std::uint64_t bytes) {
        wddm_native_buffer_create value{}; value.version=WDDM_APERTURE_VERSION; value.native_fd=fd; value.bytes=bytes;
        call(WDDM_NATIVE_BUFFER_CREATE,&value); Fd descriptor(value.fd);
        if(value.version!=WDDM_APERTURE_VERSION || descriptor.get()<0 || value.native_fd!=fd || value.bytes!=bytes || value.flags || value.reserved)
            throw std::system_error(EPROTO,std::generic_category(),"Kernel native buffer publication mismatch");
        return descriptor;
    }
    std::uint64_t nativeBufferInfo(int fd) {
        wddm_native_buffer_info value{}; value.version=WDDM_APERTURE_VERSION; value.fd=fd;
        call(WDDM_NATIVE_BUFFER_INFO,&value);
        if(value.version!=WDDM_APERTURE_VERSION || value.fd!=fd || !value.bytes || value.bytes>WDDM_NATIVE_BUFFER_MAX_BYTES || value.flags || value.reserved)
            throw std::system_error(EPROTO,std::generic_category(),"Kernel native buffer info mismatch");
        return value.bytes;
    }
    wddm_native_buffer_stats nativeBufferStats() {
        wddm_native_buffer_stats value{}; value.version=WDDM_APERTURE_VERSION;
        call(WDDM_NATIVE_BUFFER_STATS,&value);
        if(value.version!=WDDM_APERTURE_VERSION || value.held>WDDM_NATIVE_BUFFER_MAX || value.bytes>WDDM_NATIVE_BUFFER_TOTAL_BYTES || value.released>value.created)
            throw std::system_error(EPROTO,std::generic_category(),"Kernel native buffer stats mismatch");
        return value;
    }
    wddm_native_buffer_caps nativeBufferCaps() {
        wddm_native_buffer_caps value{}; value.version=WDDM_APERTURE_VERSION;
        call(WDDM_NATIVE_BUFFER_CAPS,&value);
        if(value.version!=WDDM_APERTURE_VERSION || (value.flags&~WDDM_NATIVE_BUFFER_ENABLED) || value.reserved[0] || value.reserved[1])
            throw std::system_error(EPROTO,std::generic_category(),"Kernel native buffer caps mismatch");
        return value;
    }
    ObjectRef reap() {
        wddm_object_reap value{}; value.version=WDDM_APERTURE_VERSION;
        try { call(WDDM_OBJECT_REAP,&value); }
        catch (const std::system_error& error) { if (error.code().value()==EAGAIN) return {}; throw; }
        if (!value.ticket || !value.cookie || value.reserved || (value.kind!=WDDM_OBJECT_SYNC && value.kind!=WDDM_OBJECT_RESOURCE))
            throw std::system_error(EPROTO,std::generic_category(),"Broker object reap mismatch");
        return {value.kind,value.ticket,value.cookie};
    }
};
class ObjectStore {
    enum class State { Unbound, Bound, Reaped };
    struct Entry { ObjectRef ref; State state=State::Unbound; std::uint32_t source=0; };
    std::map<std::uint32_t,Entry> entries;
    const std::uint32_t resourceReferences;
    std::uint64_t exports=0,published=0,resolved=0,reaped=0,closed=0;
    static void require(bool condition,int error,const char* message) {
        if (!condition) throw std::system_error(error,std::generic_category(),message);
    }
    template<class T> static T at(const std::vector<std::uint8_t>& bytes,std::size_t offset) {
        require(offset<=bytes.size() && sizeof(T)<=bytes.size()-offset,EPROTO,"Truncated native capability receipt");
        T value{}; std::memcpy(&value,bytes.data()+offset,sizeof(value)); return value;
    }
    static std::uint32_t exportKind(driver_bridge::Op op) {
        using driver_bridge::Op;
        return op==Op::ExportSync ? WDDM_OBJECT_SYNC : op==Op::ExportResource ? WDDM_OBJECT_RESOURCE : 0;
    }
    static std::uint32_t closeKind(driver_bridge::Op op) {
        using driver_bridge::Op;
        return op==Op::CloseSharedSync ? WDDM_OBJECT_SYNC : op==Op::CloseSharedResource ? WDDM_OBJECT_RESOURCE : 0;
    }
    Entry& entry(std::uint32_t ticket,std::uint32_t kind) {
        const auto found=entries.find(ticket);
        require(found!=entries.end() && found->second.ref.kind==kind,EBADF,"Native capability ticket or type absent");
        return found->second;
    }
    const Entry& entry(std::uint32_t ticket,std::uint32_t kind) const {
        const auto found=entries.find(ticket);
        require(found!=entries.end() && found->second.ref.kind==kind,EBADF,"Native capability ticket or type absent");
        return found->second;
    }
public:
    explicit ObjectStore(driver_bridge::ResourceSharingProfile profile=driver_bridge::GuestResourceSharingProfile)
        :resourceReferences(profile.handleLimit) {
        require(driver_bridge::validCoordinatedResourceSharingProfile(profile),EPROTO,"Invalid capability reference agreement");
    }
    void admitAlias(std::uint32_t kind,int capacityError=ENOMEM) const {
        require(kind==WDDM_OBJECT_SYNC || kind==WDDM_OBJECT_RESOURCE,EPROTO,"Invalid capability reference type");
        const auto count=std::count_if(entries.begin(),entries.end(),[kind](const auto& item){return item.second.ref.kind==kind;});
        const auto limit=kind==WDDM_OBJECT_RESOURCE ? resourceReferences : driver_bridge::MaxSharedSyncHandles;
        require(entries.size()<resourceReferences+driver_bridge::MaxSharedSyncHandles && static_cast<std::size_t>(count)<limit,
                capacityError,"Recipient capability reference capacity exhausted");
    }
    std::vector<std::uint8_t> coordinatedProfile() const { return {}; }
    bool multiClient() const { return false; }
    // Only the privileged coordinator may add an independently duplicated
    // recipient NT reference after resolving a real kernel capability FD.
    void retainAlias(ObjectRef ref) {
        require(ref.ticket && ref.cookie && (ref.kind==WDDM_OBJECT_SYNC || ref.kind==WDDM_OBJECT_RESOURCE),EPROTO,"Invalid recipient alias receipt");
        admitAlias(ref.kind);
        require(!entries.count(ref.ticket),EPROTO,"Recipient alias identity collision");
        entries.emplace(ref.ticket,Entry{ref,State::Bound,0});
    }
    template<class Authority> void authorize(const std::vector<std::uint8_t>& packet,const std::vector<Fd>& fds,Authority& kernel) {
        using namespace driver_bridge;
        const auto h=at<Header>(packet,0); const auto op=static_cast<Op>(h.type);
        const auto kind=importKind(op);
        if (kind) {
            require(fds.size()==1,EPERM,"Native import requires the actual capability FD");
            const auto ref=resolve(fds[0].get(),kind,kernel);
            require(ref.ticket==h.handle,EPERM,"Native import FD does not authorize this ticket");
        } else require(fds.empty(),EPROTO,"Native packet carried unsolicited descriptors");
        const auto closing=closeKind(op);
        if (closing) require(entry(h.handle,closing).state!=State::Bound,EPERM,"Native reference still has recipient capability FDs");
    }
    void observe(const std::vector<std::uint8_t>& packet,const std::vector<std::uint8_t>& response) {
        using namespace driver_bridge;
        const auto h=at<Header>(packet,0); const auto op=static_cast<Op>(h.type);
        const auto kind=exportKind(op),closing=closeKind(op);
        if (!kind && !closing) return;
        const auto output=at<Header>(response,0);
        require(output.type==h.type && !output.padding && output.status<=0,EPROTO,"Native capability reply identity");
        if (output.status) { require(response.size()==sizeof(Header),EPROTO,"Native capability wire-error payload"); return; }
        const auto result=at<Reply>(response,sizeof(Header));
        require(response.size()==sizeof(Header)+sizeof(Reply) && !result.reserved && result.ntstatus<=0 && !result.value,
                EPROTO,"Native capability result mismatch");
        if (result.ntstatus) return;
        if (kind) {
            // A successful native export beyond the agreed capacity is a
            // protocol violation. A refused alias preflight remains ENOMEM.
            admitAlias(kind,EPROTO);
            require(output.handle && output.handle!=UINT32_MAX && !entries.count(output.handle),
                    EPROTO,"Native capability identity or budget collision");
            entries.emplace(output.handle,Entry{ObjectRef{kind,output.handle,0},State::Unbound,h.handle}); ++exports;
        } else {
            require(output.handle==h.handle && entry(h.handle,closing).state!=State::Bound,EPROTO,"Native close before recipient FD release");
            entries.erase(h.handle); ++closed;
        }
    }
    template<class Authority> std::pair<Fd,ObjectRef> create(ObjectRequest request,Authority& kernel) {
        require(!request.reserved && request.ticket && (request.kind==WDDM_OBJECT_SYNC || request.kind==WDDM_OBJECT_RESOURCE),EPROTO,"Invalid capability publication request");
        auto& value=entry(request.ticket,request.kind);
        require(value.state==State::Unbound,EPROTO,"Native capability was already published");
        auto result=kernel.create(request.ticket,request.kind);
        require(result.first.get()>=0 && result.second.ticket==request.ticket && result.second.kind==request.kind && result.second.cookie,
                EPROTO,"Kernel publication does not match native export");
        value.ref=result.second; value.state=State::Bound; ++published; return result;
    }
    template<class Authority> ObjectRef resolve(int fd,std::uint32_t kind,Authority& kernel) {
        require(kind==WDDM_OBJECT_SYNC || kind==WDDM_OBJECT_RESOURCE,EPROTO,"Unknown capability type");
        const auto ref=kernel.resolve(fd,kind); const auto& value=entry(ref.ticket,kind);
        require(value.state==State::Bound && ref.kind==kind && ref.cookie && ref.cookie==value.ref.cookie,
                EPROTO,"Kernel capability does not match retained native reference");
        ++resolved; return ref;
    }
    template<class Authority> void validate(int fd,std::uint32_t kind,Authority& kernel) { (void)resolve(fd,kind,kernel); }
    template<class Authority> ObjectRef reap(Authority& kernel) {
        const auto ref=kernel.reap();
        if (!ref.ticket) { require(!ref.kind && !ref.cookie,EPROTO,"Empty capability reap mismatch"); return {}; }
        auto& value=entry(ref.ticket,ref.kind);
        require(value.state==State::Bound && ref.cookie && value.ref.cookie==ref.cookie,EPROTO,"Reaped capability identity mismatch");
        value.state=State::Reaped; ++reaped; return ref;
    }
    template<class Authority> void syncLineage(int fd,std::uint32_t source,Authority& kernel) {
        const auto ref=resolve(fd,WDDM_OBJECT_SYNC,kernel);
        require(source && entry(ref.ticket,WDDM_OBJECT_SYNC).source==source,EPERM,"Native fence FD does not own this monitored-fence word");
    }
    template<class Native,class Authority> void drain(Native& native,Authority& kernel) {
        std::fprintf(stderr,"BROKER_OBJECT_DRAIN producerExited=true retained=%zu\n",entries.size());
        while (!entries.empty()) {
            (void)reap(kernel);
            bool progress=false;
            for (auto i=entries.begin();i!=entries.end();) {
                if (i->second.state==State::Bound) { ++i; continue; }
                const auto ref=i->second.ref; ++i;
                const driver_bridge::Header h{static_cast<std::uint32_t>(ref.kind==WDDM_OBJECT_SYNC ? driver_bridge::Op::CloseSharedSync : driver_bridge::Op::CloseSharedResource),ref.ticket,0,0};
                const auto packet=encode(h),reply=native.exchange(packet);
                observe(packet,reply);
                require(!entries.count(ref.ticket),EIO,"Native reference cleanup did not complete");
                std::fprintf(stderr,"BROKER_OBJECT_CLOSE producerExited=true kind=%u ticket=%u cookie=%llu\n",ref.kind,ref.ticket,static_cast<unsigned long long>(ref.cookie));
                progress=true;
            }
            if (!progress && !entries.empty()) poll(nullptr,0,10);
        }
    }
    void report() const {
        std::fprintf(stderr,"BROKER_OBJECT_SUMMARY exported=%llu published=%llu resolved=%llu reaped=%llu closed=%llu retained=%zu\n",
            static_cast<unsigned long long>(exports),static_cast<unsigned long long>(published),static_cast<unsigned long long>(resolved),
            static_cast<unsigned long long>(reaped),static_cast<unsigned long long>(closed),entries.size());
    }
    std::size_t size() const { return entries.size(); }
};
} // namespace guest_broker
