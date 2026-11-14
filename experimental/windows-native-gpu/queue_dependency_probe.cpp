// SPDX-License-Identifier: MIT
// Local dependency diagnostic for the observed NVIDIA driver version ONLY.
// Recreates a captured allocation and GPU VA, then replaces the observed
// allocation token at private offset 36. Never use this layout rule in the
// bridge: return the translated token to the live UMD to build its own data.
// No rendering or command submission; successfully created objects are destroyed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "whp_fence_test.h"
#include "qemu_fence_test.h"
using Microsoft::WRL::ComPtr;
struct ContextHeader { std::uint32_t magic, node, engine, flags, clientHint, bytes; };
int main(int argc,char** argv) {
    if(argc>=2 && std::strcmp(argv[1],"--whp-fence-child")==0)
        return whp_fence::childMain(argc,argv);
    const bool whpFences=argc==6 && std::strcmp(argv[5],"--whp-fences")==0;
    const bool qemuFences=argc==11 && std::strcmp(argv[5],"--qemu-fences")==0;
    qemu_fence::Paths qemuPaths{};
    if(qemuFences) { qemuPaths={argv[6],argv[7],argv[8],argv[9],argv[10]}; argc=5; }
    if(whpFences) --argc;
    if(argc!=5) {
        std::fputs("Usage: queue-dependency-probe context-input queue-input allocation-input allocation-map "
            "[--whp-fences | --qemu-fences qemu firmware kernel initramfs new-log]\n",stderr);
        return 2;
    }
    FILE* file=nullptr;
    if(fopen_s(&file,argv[1],"rb") || !file) return 2;
    ContextHeader input{};
    if(std::fread(&input,1,sizeof input,file)!=sizeof input || input.magic!=0x31585443 ||
       !input.bytes || input.bytes>4096 || input.node>7 || input.engine!=1 ||
       (input.flags!=0 && input.flags!=16) || input.clientHint!=12) { std::fclose(file); return 2; }
    std::vector<unsigned char> payload(input.bytes);
    const bool valid=std::fread(payload.data(),1,payload.size(),file)==payload.size() && std::fgetc(file)==EOF;
    std::fclose(file); if(!valid) return 2;
    std::vector<unsigned char> queuePayload;
    if(argc>=3) {
        if(fopen_s(&file,argv[2],"rb") || !file) return 2;
        std::uint32_t header[4]{};
        if(std::fread(header,1,sizeof header,file)!=sizeof header || header[0]!=0x31554551 ||
           header[2]!=0 || header[3]>4096) { std::fclose(file); return 2; }
        queuePayload.resize(header[3]);
        const bool qvalid=std::fread(queuePayload.data(),1,queuePayload.size(),file)==queuePayload.size() && std::fgetc(file)==EOF;
        std::fclose(file); if(!qvalid) return 2;
    }
    std::uint32_t allocHeader[8]{};
    std::uint64_t mapHeader[8]{};
    if(fopen_s(&file,argv[3],"rb") || !file) return 2;
    if(std::fread(allocHeader,1,sizeof allocHeader,file)!=sizeof allocHeader || allocHeader[0]!=0x31434c41 ||
       allocHeader[1]!=0 || allocHeader[2]!=4 || !allocHeader[5] || allocHeader[5]>4096 || allocHeader[6] || allocHeader[7]) {
        std::fclose(file); return 2;
    }
    std::vector<unsigned char> allocationPayload(allocHeader[5]);
    bool validAllocation=std::fread(allocationPayload.data(),1,allocationPayload.size(),file)==allocationPayload.size() && std::fgetc(file)==EOF;
    std::fclose(file); if(!validAllocation) return 2;
    if(fopen_s(&file,argv[4],"rb") || !file) return 2;
    validAllocation=std::fread(mapHeader,1,sizeof mapHeader,file)==sizeof mapHeader && std::fgetc(file)==EOF;
    std::fclose(file);
    if(!validAllocation || mapHeader[0]!=0x3150414d || mapHeader[2] || mapHeader[3]!=16 || mapHeader[6]>3) return 2;
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    LUID luid{}; bool found=false;
    for(UINT n=0;;++n) {
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(n,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if(!adapter || FAILED(adapter->GetDesc1(&desc))) return 1;
        if(desc.VendorId==0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            luid=desc.AdapterLuid; found=true;
            std::printf("{\"vendor\":%u,\"device\":%u,",desc.VendorId,desc.DeviceId); break;
        }
    }
    if(!found) return 1;
    D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid=luid;
    auto status=D3DKMTOpenAdapterFromLuid(&adapter);
    std::printf("\"openStatus\":\"%08lx\",",static_cast<unsigned long>(status));
    if(status<0) { std::puts("\"success\":false}"); return 1; }
    D3DKMT_CREATEDEVICE device{}; device.hAdapter=adapter.hAdapter;
    const unsigned flags=2; static_assert(sizeof flags==sizeof device.Flags);
    std::memcpy(&device.Flags,&flags,sizeof flags);
    status=D3DKMTCreateDevice(&device);
    std::printf("\"deviceStatus\":\"%08lx\",",static_cast<unsigned long>(status));
    bool ok=status>=0;
    if(ok) {
        D3DKMT_CREATECONTEXTVIRTUAL context{};
        context.hDevice=device.hDevice; context.NodeOrdinal=input.node; context.EngineAffinity=input.engine;
        context.Flags.Value=input.flags; context.ClientHint=static_cast<D3DKMT_CLIENTHINT>(input.clientHint);
        context.PrivateDriverDataSize=input.bytes; context.pPrivateDriverData=payload.data();
        status=D3DKMTCreateContextVirtual(&context);
        std::printf("\"node\":%u,\"contextFlags\":%u,\"privateBytes\":%u,\"contextStatus\":\"%08lx\",",
                    input.node,input.flags,input.bytes,static_cast<unsigned long>(status));
        ok=status>=0;
        if(status>=0) {
            if(argc>=3) {
                D3DKMT_CREATEPAGINGQUEUE paging{}; paging.hDevice=device.hDevice; paging.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
                status=D3DKMTCreatePagingQueue(&paging);
                std::printf("\"pagingQueueStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                bool ready=status>=0;
                D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value=allocHeader[2]; info.Priority=allocHeader[3];
                info.VidPnSourceId=allocHeader[4]; info.pPrivateDriverData=allocationPayload.data(); info.PrivateDriverDataSize=allocHeader[5];
                D3DKMT_CREATEALLOCATION allocation{}; allocation.hDevice=device.hDevice;
                allocation.NumAllocations=1; allocation.pAllocationInfo2=&info;
                if(ready) {
                    status=D3DKMTCreateAllocation2(&allocation); ready=status>=0;
                    std::printf("\"privateAllocationStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                }
                const auto wait=[&](UINT64 target) {
                    const auto address=static_cast<volatile UINT64*>(paging.FenceValueCPUVirtualAddress);
                    const auto deadline=GetTickCount64()+5000;
                    if(!address) return false;
                    while(*address<target && GetTickCount64()<deadline) Sleep(1);
                    MemoryBarrier(); return *address>=target;
                };
                if(ready) {
                    D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue=paging.hPagingQueue;
                    resident.NumAllocations=1; resident.AllocationList=&info.hAllocation;
                    status=D3DKMTMakeResident(&resident); ready=status>=0 && wait(resident.PagingFenceValue);
                    std::printf("\"makeResidentStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                }
                if(ready) {
                    D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue=paging.hPagingQueue; map.hAllocation=info.hAllocation;
                    map.BaseAddress=mapHeader[1]; map.OffsetInPages=mapHeader[2]; map.SizeInPages=mapHeader[3];
                    map.MinimumAddress=mapHeader[4]; map.MaximumAddress=mapHeader[5]; map.Protection.Value=mapHeader[6]; map.DriverProtection=mapHeader[7];
                    status=D3DKMTMapGpuVirtualAddress(&map); ready=status>=0 && wait(map.PagingFenceValue) && map.VirtualAddress==mapHeader[1];
                    std::printf("\"mapGpuAddressStatus\":\"%08lx\",\"queueBackingReady\":%s,",static_cast<unsigned long>(status),ready?"true":"false");
                }
                D3DKMT_CREATEHWQUEUE queue{}; queue.hHwContext=context.hContext;
                if(ready && queuePayload.size()==180) {
                    D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE translation{};
                    translation.EscapeType=D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE;
                    translation.hAllocation=info.hAllocation;
                    D3DKMT_ESCAPE escape{}; escape.hAdapter=adapter.hAdapter; escape.hDevice=device.hDevice;
                    escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE; escape.Flags.DriverKnownEscape=1;
                    escape.pPrivateDriverData=&translation; escape.PrivateDriverDataSize=sizeof translation;
                    status=D3DKMTEscape(&escape); ready=status>=0;
                    std::printf("\"translateAllocationStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                    // Diagnostic for this captured driver version ONLY: the
                    // read-only trace matched offset 36 to the translated
                    // allocation token. Production must return the token to
                    // the live UMD, which then builds its own private data.
                    if(ready) std::memcpy(queuePayload.data()+36,&translation.hAllocation,4);
                }
                queue.PrivateDriverDataSize=static_cast<UINT>(queuePayload.size());
                queue.pPrivateDriverData=queuePayload.empty()?nullptr:queuePayload.data();
                status=ready?D3DKMTCreateHwQueue(&queue):static_cast<NTSTATUS>(0xc0000001u); ok=ok && status>=0;
                std::printf("\"queuePrivateBytes\":%u,\"queueStatus\":\"%08lx\",",queue.PrivateDriverDataSize,static_cast<unsigned long>(status));
                if(status>=0) {
                    const bool mapped=queue.HwQueueProgressFenceCPUVirtualAddress && queue.HwQueueProgressFenceGPUVirtualAddress && queue.hHwQueueProgressFence;
                    const auto fence=mapped?*static_cast<volatile UINT64*>(queue.HwQueueProgressFenceCPUVirtualAddress):0;
                    std::printf("\"queueFenceMapped\":%s,\"initialQueueFence\":%llu,",mapped?"true":"false",fence);
                    if(whpFences) {
                        auto pagingFence=static_cast<volatile UINT64*>(paging.FenceValueCPUVirtualAddress);
                        auto queueFence=static_cast<volatile UINT64*>(queue.HwQueueProgressFenceCPUVirtualAddress);
                        const auto pagingValue=pagingFence?*pagingFence:0;
                        const auto pagingRead=whp_fence::runChild(pagingFence,whp_fence::Mode::Read);
                        const auto queueRead=whp_fence::runChild(queueFence,whp_fence::Mode::Read);
                        const auto unmapped=whp_fence::runChild(pagingFence,whp_fence::Mode::UnmappedRead);
                        const auto pagingWrite=whp_fence::runChild(pagingFence,whp_fence::Mode::ReadOnlyWrite);
                        const auto queueWrite=whp_fence::runChild(queueFence,whp_fence::Mode::ReadOnlyWrite);
                        std::printf("\"pagingFenceValue\":%llu,\"pagingFenceGuestReadExit\":%lu,"
                            "\"queueFenceGuestReadExit\":%lu,\"unmappedReadControlExit\":%lu,"
                            "\"pagingFenceWriteDeniedExit\":%lu,\"queueFenceWriteDeniedExit\":%lu,",
                            pagingValue,pagingRead,queueRead,unmapped,pagingWrite,queueWrite);
                        ok=ok && pagingValue && !pagingRead && !queueRead && !unmapped && !pagingWrite && !queueWrite;
                    }
                    ok=ok && mapped;
                    if(qemuFences) {
                        const auto guest=qemu_fence::run(static_cast<volatile UINT64*>(paging.FenceValueCPUVirtualAddress),
                            static_cast<volatile UINT64*>(queue.HwQueueProgressFenceCPUVirtualAddress),qemuPaths);
                        std::printf("\"qemuFenceGuestExit\":%lu,",guest);
                        ok=ok && guest==0;
                    }
                    D3DKMT_DESTROYHWQUEUE dq{}; dq.hHwQueue=queue.hHwQueue;
                    status=D3DKMTDestroyHwQueue(&dq); ok=ok && status>=0;
                    std::printf("\"destroyQueueStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                }
                if(info.hAllocation) {
                    D3DKMT_DESTROYALLOCATION2 destroyAllocation{}; destroyAllocation.hDevice=device.hDevice;
                    destroyAllocation.phAllocationList=&info.hAllocation; destroyAllocation.AllocationCount=1; destroyAllocation.Flags.SynchronousDestroy=1;
                    status=D3DKMTDestroyAllocation2(&destroyAllocation); ok=ok && status>=0;
                    std::printf("\"destroyAllocationStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                }
                if(paging.hPagingQueue) {
                    D3DDDI_DESTROYPAGINGQUEUE destroyPaging{}; destroyPaging.hPagingQueue=paging.hPagingQueue;
                    status=D3DKMTDestroyPagingQueue(&destroyPaging); ok=ok && status>=0;
                    std::printf("\"destroyPagingQueueStatus\":\"%08lx\",",static_cast<unsigned long>(status));
                }
            }
            D3DKMT_DESTROYCONTEXT destroy{}; destroy.hContext=context.hContext;
            status=D3DKMTDestroyContext(&destroy); ok=ok && status>=0;
            std::printf("\"destroyContextStatus\":\"%08lx\",",static_cast<unsigned long>(status));
        }
        D3DKMT_DESTROYDEVICE destroy{}; destroy.hDevice=device.hDevice;
        status=D3DKMTDestroyDevice(&destroy); ok=ok && status>=0;
        std::printf("\"destroyDeviceStatus\":\"%08lx\",",static_cast<unsigned long>(status));
    }
    D3DKMT_CLOSEADAPTER close{}; close.hAdapter=adapter.hAdapter;
    status=D3DKMTCloseAdapter(&close); ok=ok && status>=0;
    std::printf("\"closeStatus\":\"%08lx\",\"success\":%s}\n",static_cast<unsigned long>(status),ok?"true":"false");
    return ok?0:1;
}
