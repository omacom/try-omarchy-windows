// SPDX-License-Identifier: MIT
// Consumed once by the owned same-thread shared allocation. Never infer a
// buffer description from opaque vendor/runtime bytes.
#pragma once
#include "driver_wire.h"
#include <optional>

namespace driver_bridge {
class NativeBufferPreparations {
    struct Entry { NativeGpuBufferDesc buffer; std::map<std::uint32_t, std::uint32_t> devices; };
    std::map<std::uint64_t, Entry> pending;
public:
    bool empty() const { return pending.empty(); }
    bool contains(std::uint64_t thread) const { return pending.count(thread) != 0; }
    bool pendingForDevice(std::uint32_t device) const {
        return std::any_of(pending.begin(), pending.end(), [device](const auto& item) { return item.second.devices.count(device) != 0; });
    }
    bool stage(std::uint64_t thread, NativeGpuBufferDesc buffer, const std::map<std::uint32_t, std::uint32_t>& devices) {
        if (!thread || !validNativeGpuBuffer(buffer) || devices.empty() || devices.size() > GuestOpenGlCompositorProfile.objectLimit ||
            pending.size() >= 16 || contains(thread) || std::any_of(devices.begin(), devices.end(), [](const auto& item) { return !item.first || !item.second; })) return false;
        return pending.emplace(thread, Entry{buffer, devices}).second;
    }
    std::optional<NativeGpuBufferDesc> take(std::uint64_t thread, std::uint32_t device, const std::map<std::uint32_t, std::uint32_t>& current) {
        const auto found = pending.find(thread);
        if (found == pending.end()) return std::nullopt;
        auto entry = std::move(found->second); pending.erase(found);
        const auto original = entry.devices.find(device);
        const auto live = current.find(device);
        if (original == entry.devices.end() || live == current.end() || original->second != live->second) return std::nullopt;
        return entry.buffer;
    }
    void cancel(std::uint64_t thread) { pending.erase(thread); }
    void destroyDevice(std::uint32_t device) {
        for (auto it = pending.begin(); it != pending.end();) {
            it->second.devices.erase(device);
            if (it->second.devices.empty()) it = pending.erase(it); else ++it;
        }
    }
};
}
