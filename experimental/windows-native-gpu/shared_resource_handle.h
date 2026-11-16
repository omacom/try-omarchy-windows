// SPDX-License-Identifier: MIT
// Owned native resource reference; no Windows HANDLE crosses the guest wire.
#pragma once
#include <utility>
#include "shared_nt_reference.h"

namespace driver_shared {
class ResourceHandle {
    HANDLE value = nullptr;
public:
    ResourceHandle() = default;
    ResourceHandle(const ResourceHandle&) = delete;
    ResourceHandle& operator=(const ResourceHandle&) = delete;
    ResourceHandle(ResourceHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~ResourceHandle() { if (value) CloseHandle(value); }
    HANDLE get() const { return value; }
    NTSTATUS duplicate(HANDLE source) { return duplicateNtReference(source,value); }
    NTSTATUS share(D3DKMT_HANDLE object) {
        if (value || !object) return static_cast<NTSTATUS>(0xc000000du);
        OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof attributes;
        HANDLE exported = nullptr;
        const auto status = D3DKMTShareObjects(1, &object, &attributes, GENERIC_ALL, &exported);
        if (status != 0 || !exported || exported == INVALID_HANDLE_VALUE) {
            if (exported && exported != INVALID_HANDLE_VALUE) CloseHandle(exported);
            return status < 0 ? status : static_cast<NTSTATUS>(0xc0000001u);
        }
        value = exported;
        return 0;
    }
    bool close() {
        if (!value) return true;
        if (!CloseHandle(value)) return false;
        value = nullptr;
        return true;
    }
};
}
