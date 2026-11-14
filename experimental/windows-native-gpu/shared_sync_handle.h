// SPDX-License-Identifier: MIT
// Native lifetime of one NT-shareable monitored fence. Never send HANDLEs
// or CPU/GPU virtual addresses through the guest's object-FD representation.
#pragma once
#include <stdexcept>
#include <utility>
#include "shared_nt_reference.h"

namespace driver_shared {
class SyncHandle {
    HANDLE value = nullptr;
public:
    SyncHandle() = default;
    SyncHandle(const SyncHandle&) = delete;
    SyncHandle& operator=(const SyncHandle&) = delete;
    SyncHandle(SyncHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~SyncHandle() { if (value) CloseHandle(value); }
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
    NTSTATUS open(D3DKMT_HANDLE device, UINT flags, UINT affinity,
                  D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2& result) const {
        result = {};
        if (!value || !device || (flags != 0 && flags != 3 && flags != 128 && flags != 131 && flags != 1155) || affinity > 1)
            return static_cast<NTSTATUS>(0xc000000du);
        result.hNtHandle = value; result.hDevice = device; result.Flags.Value = flags;
        result.MonitoredFence.EngineAffinity = affinity;
        return D3DKMTOpenSyncObjectFromNtHandle2(&result);
    }
    bool close() {
        if (!value) return true;
        if (!CloseHandle(value)) return false;
        value = nullptr;
        return true;
    }
};
}
