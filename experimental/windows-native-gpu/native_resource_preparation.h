// SPDX-License-Identifier: MIT
// Called under the bridge request mutex. Metadata is consumed by the actual
// same-thread KMT allocation, whose device must belong to the original snapshot.
#pragma once
#include "driver_wire.h"
#include <optional>

namespace driver_bridge {
class NativeResourcePreparations {
    struct Entry { NativeTextureDesc texture; std::map<std::uint32_t,std::uint32_t> devices; };
    std::map<std::uint64_t,Entry> pending;
public:
    bool empty() const { return pending.empty(); }
    bool contains(std::uint64_t thread) const { return pending.count(thread) != 0; }
    bool stage(std::uint64_t thread, NativeTextureDesc texture,
               const std::map<std::uint32_t,std::uint32_t>& devices) {
        if (!thread || !validNativeInteropTexture(texture) || devices.empty() ||
            devices.size() > GuestOpenGlCompositorProfile.objectLimit || pending.size() >= 16 || contains(thread) ||
            std::any_of(devices.begin(),devices.end(),[](const auto& d) { return !d.first || !d.second; })) return false;
        return pending.emplace(thread,Entry{texture,devices}).second;
    }
    std::optional<NativeTextureDesc> take(std::uint64_t thread, std::uint32_t device,
                                        const std::map<std::uint32_t,std::uint32_t>& current) {
        const auto found=pending.find(thread);
        if (found==pending.end()) return std::nullopt;
        // An attempted allocation consumes its declaration even when invalid.
        auto entry=std::move(found->second); pending.erase(found);
        const auto original=entry.devices.find(device);
        const auto live=current.find(device);
        if (original==entry.devices.end() || live==current.end() || original->second!=live->second) return std::nullopt;
        return entry.texture;
    }
    void cancel(std::uint64_t thread) { pending.erase(thread); }
    void destroyDevice(std::uint32_t device) {
        for (auto it=pending.begin();it!=pending.end();) {
            it->second.devices.erase(device);
            if (it->second.devices.empty()) it=pending.erase(it); else ++it;
        }
    }
};
}
