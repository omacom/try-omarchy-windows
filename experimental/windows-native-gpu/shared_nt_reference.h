// SPDX-License-Identifier: MIT
// Trusted owner-only NT alias. Never consumes a guest-supplied Windows handle.
#pragma once
namespace driver_shared {
inline NTSTATUS duplicateNtReference(HANDLE source,HANDLE& destination) {
    if(destination || !source || source==INVALID_HANDLE_VALUE) return static_cast<NTSTATUS>(0xc000000du);
    HANDLE alias=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS))
        return static_cast<NTSTATUS>(0xc0000001u);
    DWORD flags=0;
    if(!alias || alias==INVALID_HANDLE_VALUE || !GetHandleInformation(alias,&flags) || (flags&HANDLE_FLAG_INHERIT)) {
        if(alias && alias!=INVALID_HANDLE_VALUE) CloseHandle(alias);
        return static_cast<NTSTATUS>(0xc0000001u);
    }
    destination=alias;return 0;
}
}
