// SPDX-License-Identifier: MIT
// Optional reduction of successful ioctl traces before they reach journald.
// This is a function, not a macro: caller arguments keep their evaluation.
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef WDDM_BRIDGE_VERBOSE_IOCTL_TRACE
#define WDDM_BRIDGE_VERBOSE_IOCTL_TRACE 1
#endif
#if WDDM_BRIDGE_VERBOSE_IOCTL_TRACE != 0 && WDDM_BRIDGE_VERBOSE_IOCTL_TRACE != 1
#error Ioctl trace selection must be zero or one
#endif

namespace guest_bridge_log {
inline std::atomic<std::uint64_t> suppressed{0};
inline bool containsInsensitive(const char* text,const char* token) {
    for(;*text;++text) {
        const char* candidate=text;const char* wanted=token;
        while(*candidate && *wanted) {
            const unsigned char c=static_cast<unsigned char>(*candidate);
            const unsigned char lower=c>='A' && c<='Z' ? c+('a'-'A') : c;
            if(lower!=static_cast<unsigned char>(*wanted)) break;
            ++candidate;++wanted;
        }
        if(!*wanted) return true;
    }
    return false;
}
inline bool retained(const char* format) {
    // Unknown/non-bridge messages remain visible. Status/result formats are
    // retained even on success, so a dynamic failure is never classified away.
    if(std::strncmp(format,"LINUX_BRIDGE ",13)) return true;
    constexpr const char* tokens[]={"error","errno","fail","unsupported","reject",
        "refus","timeout","status","result","summary","worker","probe","test",
        "vendor=","umdpresent","profilenegotiated","processexitviewsretained"};
    for(const char* token:tokens) if(containsInsensitive(format,token)) return true;
    // Suppress only inspected routine record names, never an unknown future
    // bridge diagnostic. A longer name sharing a prefix is not the same record.
    constexpr const char* routine[]={"allocationCpuMapped","allocationCpuUnmapped",
        "allocationDriverAliasCreated","allocationInput","allocationItem",
        "contextInput","contextPriorityInput","contextSignalContext","contextSignalInput","contextSignalObject",
        "cpuFenceWaitInput","cpuNotificationInput","evictionAllocation","evictionInput",
        "gpuVaAllocationInput","gpuVaInput","gpuWaitOwnership","guestFenceUnmapped",
        "hwQueueDescriptor","hwQueueInput","hwQueueSignalInput","kernelApertureLease","kernelApertureRevoked",
        "kernelObjectExported","kernelObjectReaped","knownAllocationTranslation","knownEscapeInput","lock2Input",
        "memoryBudgetInput","monitoredFenceInput","nativeContextCreated","nativeContextPriorityChanged",
        "nativeContextSignalSubmitted","nativeGpuQueueWaitQueued","nativeGpuReservationFreed","nativeGpuReserved",
        "nativeHwQueueCreated","nativeHwQueueDestroyed","nativeHwQueueSignalQueued","nativeInteropResource",
        "nativeMemoryBudget","nativeResourceImportForwarded","nativeResourceImportInput","nativeResourceInfoQueried",
        "nativeShareObjectInput","nativeSyncFdImportInput","nativeSyncFdImported",
        "nativeSynchronizationCreated","nativeSynchronizationDestroyed","nativeVendorAllocationCreated",
        "nativeVendorAllocationsDestroyed","nativeVendorCpuLocked","nativeVendorCpuUnlocked",
        "nativeVendorDriverProtectionMapped","nativeVendorEviction","nativeVendorResourceCreated","nativeVendorResourceDestroyed",
        "pagingFenceRetired","query","queryCompleted","recipientReferenceAdopted","reserveGpuVaInput","residentInput",
        "shareAttributes","standaloneSourceUninitializedAccepted","submitInput","submitOwnership","synchronizationInput"};
    for(const char* record:routine) {
        const auto bytes=std::strlen(record);
        if(!std::strncmp(format+13,record,bytes) &&
            (format[13+bytes]=='=' || format[13+bytes]==' ' || format[13+bytes]=='\n' || !format[13+bytes])) return false;
    }
    return true;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,2,3)))
#endif
inline int diagnostic(std::FILE* stream,const char* format,...) {
    if constexpr(!WDDM_BRIDGE_VERBOSE_IOCTL_TRACE) {
        if(!retained(format)) {
            suppressed.fetch_add(1,std::memory_order_relaxed);
            return 0;
        }
    }
    va_list arguments;va_start(arguments,format);
    const int result=std::vfprintf(stream,format,arguments);
    va_end(arguments);return result;
}
inline void report(std::FILE* stream=stderr) {
    if constexpr(!WDDM_BRIDGE_VERBOSE_IOCTL_TRACE) {
        std::fprintf(stream,"LINUX_BRIDGE diagnosticLogSummary routineTraceEnabled=0 suppressedRoutineRecords=%llu statusAndErrorFormatsRetained=true\n",
            static_cast<unsigned long long>(suppressed.load(std::memory_order_relaxed)));
    }
}
}
