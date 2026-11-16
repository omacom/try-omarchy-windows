// SPDX-License-Identifier: MIT
// View authority derives exclusively from successful native wire receipts.
#pragma once
#include "driver_wire.h"
#include "driver_coordinator.h"
#include "guest_broker_client.h"
#include <map>
#include <set>

namespace guest_broker {
class Views {
public:
    struct Grant { ViewRequest view; std::uint64_t cookie = 0; std::uint32_t fenceOffset=0; };
private:
    std::map<std::uint32_t, Grant> grants;
    std::map<std::uint32_t, std::uint32_t> parents, resources;
    std::size_t objectLimit=512;
    bool profileObserved=false;
    bool sparseAperture=false;
    bool desktopSlots=false;
    std::uint64_t fenceApertureBytes=driver_bridge::FenceApertureBytes;
    static void require(bool condition, const char* message) {
        if (!condition) throw std::system_error(EPROTO, std::generic_category(), message);
    }
    template<class T> static T at(const std::vector<std::uint8_t>& packet, std::size_t offset) {
        require(offset <= packet.size() && sizeof(T) <= packet.size() - offset, "Truncated native receipt");
        T value{}; std::memcpy(&value, packet.data() + offset, sizeof(value)); return value;
    }
    void parent(std::uint32_t object, std::uint32_t owner) {
        require(object && object != owner && !parents.count(object), "Native object identity collision");
        require(parents.size() < objectLimit, "Native object quota exhausted");
        if (owner) require(parents.count(owner) != 0, "Native object parent absent");
        parents.emplace(object, owner);
    }
    bool below(std::uint32_t object, std::uint32_t owner) const {
        for (std::size_t n = 0; object && n <= parents.size(); ++n) {
            if (object == owner) return true;
            const auto found = parents.find(object);
            if (found == parents.end()) return false;
            object = found->second;
        }
        return false;
    }
    std::vector<std::uint32_t> retiring(const std::vector<std::uint8_t>& packet) const {
        using namespace driver_bridge;
        const auto h = at<Header>(packet, 0); const auto op = static_cast<Op>(h.type);
        if (op == Op::DestroyVendorAllocations) {
            const auto desc = at<DestroyVendorDesc>(packet, sizeof(h));
            require(desc.count && desc.count <= ExpandedVendorAllocations &&
                    packet.size() == sizeof(h) + sizeof(desc) + desc.count * 4, "Malformed native allocation destruction");
            std::vector<std::uint32_t> ids(desc.count);
            std::memcpy(ids.data(), packet.data() + sizeof(h) + sizeof(desc), desc.count * 4); return ids;
        }
        if (op == Op::DestroyVendorResource) {
            const auto found = resources.find(h.handle);
            return {found == resources.end() ? h.handle : found->second};
        }
        if (op == Op::DestroyPagingQueue || op == Op::DestroyHwQueue || op == Op::DestroySync ||
            op == Op::UnlockVendorAllocation || op == Op::DestroyDevice || op == Op::DestroyContext || op == Op::CloseAdapter)
            return {h.handle};
        return {};
    }
    void grant(std::uint32_t object, ViewRequest view, std::uint32_t fenceOffset=0) {
        using namespace driver_bridge;
        require(parents.count(object) && grants.size() < WDDM_APERTURE_MAX_LEASES && !grants.count(object), "Native view identity collision");
        if (view.kind == WDDM_APERTURE_FENCE)
            require(!view.reserved && view.bytes == 4096 && !(view.offset % 4096) && view.offset < fenceApertureBytes && !view.generation,
                    "Native fence view out of bounds");
        else require(view.kind == WDDM_APERTURE_ALLOCATION && !view.reserved &&
                     validVendorCpuReply(view.offset, VendorCpuReply{static_cast<std::uint32_t>(view.bytes),0,view.generation},sparseAperture,desktopSlots),
                     "Native allocation view out of bounds");
        for (const auto& other : grants) {
            const auto& b = other.second.view;
            require(view.kind != b.kind || view.offset >= b.offset + b.bytes || b.offset >= view.offset + view.bytes,
                    "Native view overlap");
        }
        grants.emplace(object, Grant{view,0,fenceOffset});
    }
public:
    void approve(const std::vector<std::uint8_t>& packet) const {
        using namespace driver_bridge;
        require(packet.size() >= sizeof(Header) && packet.size() <= native_gpu::MaxPacket, "Native packet bounds");
        const auto h = at<Header>(packet,0);
        require(!h.status && !h.padding, "Invalid native request header");
        for (const auto target : retiring(packet)) for (const auto& entry : grants)
            if (entry.second.cookie && below(entry.first,target))
                throw std::system_error(EBUSY,std::generic_category(),"Native owner still has a CPU view");
    }
    void observe(const std::vector<std::uint8_t>& request, const std::vector<std::uint8_t>& response,
                 const driver_bridge::coordinator::Info* agreement=nullptr) {
        using namespace driver_bridge;
        const auto h = at<Header>(request,0), r = at<Header>(response,0);
        require(response.size() <= native_gpu::MaxPacket && r.type == h.type && !r.padding && r.status <= 0,
                "Native reply identity mismatch");
        if (r.status) { require(response.size() == sizeof(Header),"Native wire error payload"); return; }
        const auto op = static_cast<Op>(h.type);
        if(op==Op::BeginOpenGl) {
            require(!h.handle && !r.handle && !profileObserved && parents.empty() && grants.empty() && resources.empty() &&
                    (request.size()==sizeof(Header) || request.size()==sizeof(Header)+sizeof(std::uint32_t) || request.size()==sizeof(Header)+sizeof(OpenGlDesktop4kCapacity) || request.size()==sizeof(Header)+sizeof(OpenGlDesktopLargeMapCapacity) || request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity)) &&
                    response.size()==sizeof(Header)+sizeof(OpenGlProfile),"Invalid native OpenGL profile receipt");
            const auto version=request.size()==sizeof(Header) ? 1u : at<std::uint32_t>(request,sizeof(Header));
            require(version==14 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity) &&
                    (validOpenGlMultiappCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header))) ||
                     (agreement && coordinator::matchesApplicationMemoryCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header)),*agreement))) : version==13 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity) &&
                    validOpenGlDesktopReferenceCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header))) : version==12 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity) &&
                    validOpenGlDesktopSlotCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header))) : version==11 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity) &&
                    validOpenGlDesktopFenceCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header))) : version==10 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopPresentationCapacity) &&
                    validOpenGlDesktopPresentationCapacity(at<OpenGlDesktopPresentationCapacity>(request,sizeof(Header))) : version==9 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktopLargeMapCapacity) &&
                    validOpenGlDesktopLargeMapCapacity(at<OpenGlDesktopLargeMapCapacity>(request,sizeof(Header))) : version>=7 ? request.size()==sizeof(Header)+sizeof(OpenGlDesktop4kCapacity) &&
                    (version==7 ? validOpenGlDesktop4kCapacity(at<OpenGlDesktop4kCapacity>(request,sizeof(Header))) : version==8 && validOpenGlDesktopSparseCapacity(at<OpenGlDesktop4kCapacity>(request,sizeof(Header)))) :
                    request.size()==sizeof(Header)+(version==1 ? 0 : sizeof(std::uint32_t)),"Wrong profile request extent");
            const auto profile=at<OpenGlProfile>(response,sizeof(Header));
            require(version==1 ? validOpenGlProfile(profile) : version==2 ? validOpenGlFenceProfile(profile) :
                    version==3 ? validOpenGlCompositorProfile(profile) : version==4 ? validOpenGlCompositorRenderProfile(profile) :
                    version==5 ? validOpenGlDesktopProfile(profile) : version==6 ? validOpenGlDesktop4kProfile(profile) : version==7 ? validOpenGlDesktop4kCpuProfile(profile) : version==8 ? validOpenGlDesktopSparseProfile(profile) : version==9 ? validOpenGlDesktopLargeMapProfile(profile) : version==14 ? validOpenGlMultiappProfile(profile) : version==13 ? validOpenGlDesktopReferenceProfile(profile) : version==12 ? validOpenGlDesktopSlotProfile(profile) : version==11 ? validOpenGlDesktopFenceProfile(profile) : version==10 && validOpenGlDesktopPresentationProfile(profile),"Wrong native OpenGL profile receipt");
            // The broker must enforce the host's exact negotiated live-object
            // budget. Its former unconditional 512 cap rejected valid v4/v5
            // allocations while the native session still had capacity.
            if(version>=2) objectLimit=profile.objectLimit;
            profileObserved=true; sparseAperture=version>=8;desktopSlots=version>=12;
            fenceApertureBytes=version>=11 ? DesktopFenceApertureBytes : FenceApertureBytes;
            return;
        }
        const auto retired = retiring(request);
        const bool creating = op == Op::OpenAdapter || op == Op::CreateDevice || op == Op::CreateContext ||
            op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation ||
            op == Op::CreateSharedVendorResourceAllocation || op == Op::CreateNativeSharedVendorResourceAllocation || op == Op::CreateNativeInteropResource || op == Op::CreateNativeSharedGpuBufferAllocation ||
            op == Op::CreateGuestPagingQueue || op == Op::CreateHwQueue || op == Op::CreateSync || op == Op::LockVendorAllocation ||
            op == Op::ImportResource || op == Op::ImportSync;
        // Other operations can have non-Reply payloads or legitimate pending
        // native statuses (GPU VA mapping). They confer no CPU-view authority.
        if (!creating && retired.empty()) return;
        const auto result = at<Reply>(response,sizeof(Header));
        require(!result.reserved && result.ntstatus <= 0,"Native reply result mismatch");
        if (result.ntstatus) return;
        const auto payload = sizeof(Header) + sizeof(Reply);
        if (op == Op::OpenAdapter) parent(r.handle,0);
        if (op == Op::CreateDevice || op == Op::CreateContext) parent(r.handle,h.handle);
        if (op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation ||
            op == Op::CreateSharedVendorResourceAllocation || op == Op::CreateNativeSharedVendorResourceAllocation || op == Op::CreateNativeInteropResource || op == Op::CreateNativeSharedGpuBufferAllocation) {
            parent(r.handle,h.handle);
            if (op != Op::CreateVendorAllocation) {
                const auto resource = at<VendorResourceReply>(response,payload);
                require(!resource.reserved,"Native resource reply padding"); parent(resource.resource,r.handle);
                resources.emplace(resource.resource,r.handle);
            }
        }
        if (op == Op::CreateGuestPagingQueue) {
            const auto value = at<PagingReply>(response,payload);
            require(response.size() == payload + sizeof(value) && !value.reserved && !(value.offset % 8),"Native paging receipt");
            parent(r.handle,h.handle); parent(value.sync,r.handle);
            grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0},static_cast<std::uint32_t>(value.offset&4095));
        }
        if (op == Op::CreateHwQueue) {
            const auto value = at<HwQueueReply>(response,payload);
            const auto desc = at<HwQueueDesc>(request,sizeof(Header));
            require(validHwQueue(desc) && request.size() == sizeof(Header) + sizeof(desc) + desc.privateBytes &&
                    response.size() == payload + sizeof(value) + desc.privateBytes && !value.reserved && !(value.offset % 8),
                    "Native queue receipt");
            parent(r.handle,h.handle); parent(value.sync,r.handle);
            grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0},static_cast<std::uint32_t>(value.offset&4095));
        }
        if (op == Op::CreateSync) {
            const auto desc = at<SyncDesc>(request,sizeof(Header)); parent(r.handle,h.handle);
            if (desc.type == 5) {
                const auto value = at<SyncReply>(response,payload);
                require(response.size() == payload + sizeof(value) && !(value.offset % 8),"Native sync receipt");
                grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0},static_cast<std::uint32_t>(value.offset&4095));
            }
        }
        if (op == Op::ImportSync) {
            const auto desc=at<SharedSyncOpenDesc>(request,sizeof(Header));
            const auto value=at<SyncReply>(response,payload);
            require(request.size()==sizeof(Header)+sizeof(desc) && validSharedSyncOpen(desc) && !result.value &&
                    response.size()==payload+sizeof(value) && !(value.offset%8) && value.offset<fenceApertureBytes &&
                    validSyncGpuAddress(SyncDesc{5,desc.flags,desc.affinity,0,0},value.gpuAddress),"Native imported sync receipt");
            parent(r.handle,desc.device);
            grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset&~4095ull,4096,0},static_cast<std::uint32_t>(value.offset&4095));
        }
        if (op == Op::ImportResource) {
            const auto desc=at<SharedResourceOpenDesc>(request,sizeof(Header));
            const auto value=at<SharedResourceOpenReply>(response,payload);
            require(request.size()==sizeof(Header)+sizeof(desc) && validSharedResourceOpen(desc) && !value.reserved &&
                    response.size()==payload+sizeof(value)+desc.runtimeBytes+desc.driverBytes+desc.resourceDriverBytes &&
                    !(result.value%4096) && result.value<MaxGpuAddress && value.allocationDriverBytes &&
                    value.allocationDriverOffset<=desc.driverBytes && value.allocationDriverBytes<=desc.driverBytes-value.allocationDriverOffset,
                    "Native imported resource receipt");
            parent(r.handle,desc.device); parent(value.resource,r.handle); resources.emplace(value.resource,r.handle);
        }
        if (op == Op::LockVendorAllocation) {
            const auto value = at<VendorCpuReply>(response,payload);
            require(response.size() == payload + sizeof(value) && r.handle == h.handle && validVendorCpuReply(result.value,value,sparseAperture,desktopSlots),
                    "Native CPU lock receipt");
            grant(h.handle,{WDDM_APERTURE_ALLOCATION,0,result.value,value.bytes,value.generation});
        }
        for (const auto target : retired) {
            for (auto i = grants.begin(); i != grants.end();) {
                if (below(i->first,target)) { require(!i->second.cookie,"Native released live CPU lease"); i=grants.erase(i); }
                else ++i;
            }
            if (op != Op::UnlockVendorAllocation) {
                std::set<std::uint32_t> removed;
                for (const auto& i : parents) if (below(i.first,target)) removed.insert(i.first);
                for (const auto id : removed) { parents.erase(id); resources.erase(id); }
            }
        }
    }
    Grant& acquire(ViewRequest view) {
        require(!view.reserved,"Reserved view request field");
        for (auto& entry : grants) {
            auto& g = entry.second;
            if (g.view.kind == view.kind && g.view.offset == view.offset && g.view.bytes == view.bytes && g.view.generation == view.generation) {
                if (g.cookie) throw std::system_error(EBUSY,std::generic_category(),"Native CPU view already granted");
                return g;
            }
        }
        throw std::system_error(EPERM,std::generic_category(),"No native receipt authorizes this CPU view");
    }
    Grant& cookie(std::uint64_t cookie) {
        if (cookie) for (auto& entry : grants) if (entry.second.cookie == cookie) return entry.second;
        throw std::system_error(EBADF,std::generic_category(),"Unknown CPU view cookie");
    }
    std::size_t active() const {
        return std::count_if(grants.begin(),grants.end(),[](const auto& i){return i.second.cookie != 0;});
    }
    std::uint32_t syncFile(SyncFileRequest request,bool linked=false) {
        const auto& g=cookie(request.cookie);
        if (g.view.kind!=WDDM_APERTURE_FENCE || request.offset!=g.fenceOffset ||
            (!linked && !request.value) || request.value==UINT64_MAX)
            throw std::system_error(EPERM,std::generic_category(),"Sync-file requires the exact authorized native fence word");
        for(const auto& entry:grants) if(entry.second.cookie==request.cookie) return entry.first;
        throw std::system_error(EBADF,std::generic_category(),"Missing native fence source");
    }
};
} // namespace guest_broker
