// SPDX-License-Identifier: MIT
// Diagnostic tracing for umd_context_probe only. Captured vendor bytes stay
// local and are not a portable ABI. Public metadata identifies dependencies.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
namespace {
std::mutex traceGuard;
unsigned contextSequence = 0;
unsigned allocationSequence = 0;
unsigned querySequence = 0;
std::map<unsigned,unsigned> queryAdapterIds;
unsigned counts[256]{};
std::map<unsigned,unsigned> contextIds;
std::map<unsigned,unsigned> allocationIds;
std::vector<std::pair<std::uint64_t,std::uint64_t>> gpuRanges;
std::map<unsigned,unsigned> driverAllocationTokens;
void reportReferences(const char* label,const void* data,unsigned size) {
    if(!data || size>4096) return;
    for(unsigned i=0;i+8<=size;i+=4) {
        std::uint64_t value{}; std::memcpy(&value,static_cast<const unsigned char*>(data)+i,8);
        for(const auto& range:gpuRanges)
            if(value>=range.first && value-range.first<range.second)
                std::fprintf(stderr,"DXG %s gpuReferenceOffset=%u allocationBytes=%llu\n",label,i,static_cast<unsigned long long>(range.second));
    }
}
struct ContextHeader {
    std::uint32_t magic, node, engine, flags, clientHint, bytes;
};
struct Summary {
    ~Summary() {
        for (unsigned n = 0; n < 256; ++n)
            if (counts[n]) std::fprintf(stderr,"DXG summary nr=%u count=%u\n",n,counts[n]);
    }
} summary;
}
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args; va_start(args,request); void* data=va_arg(args,void*); va_end(args);
    using Function=int(*)(int,unsigned long,...);
    static auto real=reinterpret_cast<Function>(dlsym(RTLD_NEXT,"ioctl"));
    if(!real) return -1;
    unsigned sequence = 0;
    unsigned allocSequence = 0;
    unsigned translatedSequence = 0;
    unsigned queryNumber = 0, queryAdapter = 0;
    if(_IOC_TYPE(request)=='G' && data) {
        std::lock_guard<std::mutex> lock(traceGuard);
        ++counts[_IOC_NR(request)];
        if(_IOC_NR(request)==9 && _IOC_SIZE(request)==sizeof(D3DKMT_QUERYADAPTERINFO)) {
            const auto& a=*static_cast<D3DKMT_QUERYADAPTERINFO*>(data);
            queryNumber=++querySequence;
            auto entry=queryAdapterIds.find(a.hAdapter);
            if(entry==queryAdapterIds.end())
                entry=queryAdapterIds.emplace(a.hAdapter,static_cast<unsigned>(queryAdapterIds.size()+1)).first;
            queryAdapter=entry->second;
            std::fprintf(stderr,"DXG queryInput sequence=%u adapterSequence=%u type=%u bytes=%u\n",
                queryNumber,queryAdapter,static_cast<unsigned>(a.Type),a.PrivateDriverDataSize);
            if(a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=65536) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-query-input-%u.bin",queryNumber);
                if(auto file=std::fopen(path,"wb")) {
                    const unsigned header[]{0x31595141,queryNumber,static_cast<unsigned>(a.Type),a.PrivateDriverDataSize,queryAdapter,0};
                    std::fwrite(header,1,sizeof header,file);
                    std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
                }
            }
        } else if(_IOC_NR(request)==6) {
            if(_IOC_SIZE(request)!=sizeof(D3DKMT_CREATEALLOCATION)) {
                std::fprintf(stderr,"DXG allocation ABI bytes=%u expected=%u\n",static_cast<unsigned>(_IOC_SIZE(request)),static_cast<unsigned>(sizeof(D3DKMT_CREATEALLOCATION)));
            } else {
                const auto& a=*static_cast<D3DKMT_CREATEALLOCATION*>(data);
                const auto number=++allocationSequence;
                allocSequence=number;
                unsigned flags=0; static_assert(sizeof a.Flags == sizeof flags);
                std::memcpy(&flags,&a.Flags,sizeof flags);
                std::fprintf(stderr,"DXG allocationInput sequence=%u flags=%u count=%u runtimeBytes=%u privateBytes=%u resourceReuse=%u\n",
                    number,flags,a.NumAllocations,a.PrivateRuntimeDataSize,a.PrivateDriverDataSize,a.hResource?1u:0u);
                if(a.NumAllocations<=16 && a.pAllocationInfo2)
                    for(unsigned i=0;i<a.NumAllocations;++i) {
                        const auto& item=a.pAllocationInfo2[i];
                        std::fprintf(stderr,"DXG allocationItem sequence=%u index=%u flags=%u privateBytes=%u hasSystemMemory=%u\n",
                            number,i,item.Flags.Value,item.PrivateDriverDataSize,item.pSystemMem?1u:0u);
                    }
                if(a.NumAllocations==1 && a.pAllocationInfo2 && !a.PrivateRuntimeDataSize && !a.PrivateDriverDataSize && !flags) {
                    const auto& item=a.pAllocationInfo2[0];
                    if(!item.pSystemMem && item.PrivateDriverDataSize<=4096) {
                        char path[80]; std::snprintf(path,sizeof path,"wsl-allocation-input-%u.bin",number);
                        if(auto file=std::fopen(path,"wb")) {
                            const unsigned header[]{0x31434c41,flags,item.Flags.Value,item.Priority,item.VidPnSourceId,item.PrivateDriverDataSize,0,0};
                            std::fwrite(header,1,sizeof header,file);
                            std::fwrite(item.pPrivateDriverData,1,item.PrivateDriverDataSize,file); std::fclose(file);
                        }
                    }
                }
            }
        } else if(_IOC_NR(request)==4 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATECONTEXTVIRTUAL)) {
            const auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(data);
            sequence=++contextSequence;
            std::fprintf(stderr,"DXG contextInput sequence=%u node=%u engine=%u flags=%u hint=%u privateBytes=%u\n",
                         sequence,a.NodeOrdinal,a.EngineAffinity,a.Flags.Value,static_cast<unsigned>(a.ClientHint),a.PrivateDriverDataSize);
            if(a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-context-input-%u.bin",sequence);
                if(auto file=std::fopen(path,"wb")) {
                    const ContextHeader h{0x31585443,a.NodeOrdinal,a.EngineAffinity,a.Flags.Value,static_cast<unsigned>(a.ClientHint),a.PrivateDriverDataSize};
                    std::fwrite(&h,1,sizeof h,file);
                    std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
                }
            }
        } else if(_IOC_NR(request)==24 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATEHWQUEUE)) {
            const auto& a=*static_cast<D3DKMT_CREATEHWQUEUE*>(data);
            const auto found=contextIds.find(a.hHwContext);
            const unsigned context=found==contextIds.end()?0:found->second;
            unsigned flags=0; static_assert(sizeof a.Flags == sizeof flags);
            std::memcpy(&flags,&a.Flags,sizeof flags);
            std::fprintf(stderr,"DXG queueInput contextSequence=%u flags=%u privateBytes=%u\n",context,flags,a.PrivateDriverDataSize);
            reportReferences("queue",a.pPrivateDriverData,a.PrivateDriverDataSize);
            if(a.pPrivateDriverData && a.PrivateDriverDataSize<=4096) {
                for(unsigned offset=0;offset+4<=a.PrivateDriverDataSize;offset+=4) {
                    unsigned value=0; std::memcpy(&value,static_cast<const unsigned char*>(a.pPrivateDriverData)+offset,4);
                    const auto token=driverAllocationTokens.find(value);
                    if(value && token!=driverAllocationTokens.end())
                        std::fprintf(stderr,"DXG queue translatedAllocationReferenceOffset=%u allocationSequence=%u\n",offset,token->second);
                }
            }
            if(context && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-queue-input-%u.bin",context);
                if(auto file=std::fopen(path,"wb")) {
                    const unsigned header[]{0x31554551,context,flags,a.PrivateDriverDataSize};
                    std::fwrite(header,1,sizeof header,file);
                    if(a.PrivateDriverDataSize) std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file);
                    std::fclose(file);
                }
            }
        } else if(_IOC_NR(request)==13 && _IOC_SIZE(request)==sizeof(D3DKMT_ESCAPE)) {
            const auto& a=*static_cast<D3DKMT_ESCAPE*>(data);
            std::fprintf(stderr,"DXG escapeInput type=%u flags=%u privateBytes=%u\n",static_cast<unsigned>(a.Type),a.Flags.Value,a.PrivateDriverDataSize);
            if(a.Flags.DriverKnownEscape && a.pPrivateDriverData && a.PrivateDriverDataSize==8) {
                const auto& e=*static_cast<D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE*>(a.pPrivateDriverData);
                const auto found=allocationIds.find(e.hAllocation);
                if(found!=allocationIds.end()) translatedSequence=found->second;
                std::fprintf(stderr,"DXG knownEscape type=%u allocationSequence=%u\n",static_cast<unsigned>(e.EscapeType),found==allocationIds.end()?0:found->second);
            }
        } else if(_IOC_NR(request)==2 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATEDEVICE)) {
            const auto& a=*static_cast<D3DKMT_CREATEDEVICE*>(data);
            unsigned flags=0; static_assert(sizeof a.Flags == sizeof flags);
            std::memcpy(&flags,&a.Flags,sizeof flags);
            std::fprintf(stderr,"DXG deviceInput flags=%u\n",flags);
        }
    }
    const auto result=real(fd,request,data);
    const int savedErrno=errno;
    if(queryNumber) {
        const auto& a=*static_cast<D3DKMT_QUERYADAPTERINFO*>(data);
        std::lock_guard<std::mutex> lock(traceGuard);
        std::fprintf(stderr,"DXG queryOutput sequence=%u adapterSequence=%u type=%u bytes=%u result=%d errno=%d\n",
            queryNumber,queryAdapter,static_cast<unsigned>(a.Type),a.PrivateDriverDataSize,result,result<0?savedErrno:0);
        if(result>=0 && a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=65536) {
            char path[80]; std::snprintf(path,sizeof path,"wsl-query-output-%u.bin",queryNumber);
            if(auto file=std::fopen(path,"wb")) {
                std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
            }
        }
    }
    if(result>=0 && _IOC_TYPE(request)=='G' && _IOC_NR(request)==13 && _IOC_SIZE(request)==sizeof(D3DKMT_ESCAPE) && data) {
        const auto& a=*static_cast<D3DKMT_ESCAPE*>(data);
        if(a.Flags.DriverKnownEscape && a.pPrivateDriverData && a.PrivateDriverDataSize==8) {
            const auto& e=*static_cast<D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE*>(a.pPrivateDriverData);
            std::lock_guard<std::mutex> lock(traceGuard);
            if(translatedSequence) driverAllocationTokens[e.hAllocation]=translatedSequence;
        }
    }
    if(allocSequence && result>=0) {
        std::lock_guard<std::mutex> lock(traceGuard);
        const auto& a=*static_cast<D3DKMT_CREATEALLOCATION*>(data);
        if(a.NumAllocations==1 && a.pAllocationInfo2) allocationIds[a.pAllocationInfo2[0].hAllocation]=allocSequence;
    }
    if(result>=0 && _IOC_TYPE(request)=='G' && _IOC_NR(request)==12 && _IOC_SIZE(request)==sizeof(D3DDDI_MAPGPUVIRTUALADDRESS) && data) {
        const auto& a=*static_cast<D3DDDI_MAPGPUVIRTUALADDRESS*>(data);
        std::lock_guard<std::mutex> lock(traceGuard);
        gpuRanges.emplace_back(a.VirtualAddress,a.SizeInPages*4096);
        const auto found=allocationIds.find(a.hAllocation);
        if(found!=allocationIds.end()) {
            char path[80]; std::snprintf(path,sizeof path,"wsl-allocation-map-%u.bin",found->second);
            if(auto file=std::fopen(path,"wb")) {
                std::uint64_t protection=0; static_assert(sizeof a.Protection==sizeof protection);
                std::memcpy(&protection,&a.Protection,sizeof protection);
                const std::uint64_t header[]{0x3150414d,a.VirtualAddress,a.OffsetInPages,a.SizeInPages,a.MinimumAddress,a.MaximumAddress,protection,a.DriverProtection};
                std::fwrite(header,1,sizeof header,file); std::fclose(file);
            }
            std::fprintf(stderr,"DXG allocationMap sequence=%u pages=%llu\n",found->second,static_cast<unsigned long long>(a.SizeInPages));
        }
    }
    if(sequence) {
        std::lock_guard<std::mutex> lock(traceGuard);
        if(result>=0) {
            const auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(data);
            contextIds[a.hContext]=sequence;
            if(a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-context-output-%u.bin",sequence);
                if(auto file=std::fopen(path,"wb")) {
                    std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
                }
            }
        }
        std::fprintf(stderr,"DXG contextResult sequence=%u return=%d\n",sequence,result);
    }
    errno=savedErrno;
    return result;
}
