// SPDX-License-Identifier: MIT
// Local compatibility diagnostic. Driver-private query captures stay local.
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
using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    if(argc!=3) { std::fputs("Usage: query-native-probe input-capture local-output\n",stderr); return 2; }
    FILE* file=nullptr;
    if(fopen_s(&file,argv[1],"rb") || !file) return 2;
    std::uint32_t header[6]{};
    if(std::fread(header,1,sizeof header,file)!=sizeof header || header[0]!=0x31595141 ||
        !header[3] || header[3]>65536 || header[5]) { std::fclose(file); return 2; }
    // Only the inline query types observed in this diagnostic; no nested
    // process pointers, arbitrary registry queries or working-set mutation.
    switch(header[2]) {
        case 0: case 1: case 3: case 13: case 15: case 17: case 18: case 24:
        case 27: case 30: case 31: case 34: case 55: case 56: case 60: case 61:
        case 62: case 66: break;
        default: std::fclose(file); return 2;
    }
    std::vector<unsigned char> buffer(header[3]);
    const bool valid=std::fread(buffer.data(),1,buffer.size(),file)==buffer.size() && std::fgetc(file)==EOF;
    std::fclose(file); if(!valid) return 2;
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    LUID luid{}; bool found=false;
    for(UINT n=0;;++n) {
        ComPtr<IDXGIAdapter1> candidate;
        if(factory->EnumAdapters1(n,&candidate)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if(!candidate || FAILED(candidate->GetDesc1(&desc))) return 1;
        if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
            luid=desc.AdapterLuid; found=true;
            std::printf("{\"vendor\":%u,\"device\":%u,",desc.VendorId,desc.DeviceId); break;
        }
    }
    if(!found) return 1;
    D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid=luid;
    auto status=D3DKMTOpenAdapterFromLuid(&adapter);
    std::printf("\"openStatus\":\"%08lx\",",static_cast<unsigned long>(status));
    if(status<0) { std::puts("\"success\":false}"); return 1; }
    D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter=adapter.hAdapter;
    query.Type=static_cast<KMTQUERYADAPTERINFOTYPE>(header[2]);
    query.pPrivateDriverData=buffer.data(); query.PrivateDriverDataSize=header[3];
    status=D3DKMTQueryAdapterInfo(&query); bool ok=status>=0;
    std::printf("\"sequence\":%u,\"adapterSequence\":%u,\"type\":%u,\"bytes\":%u,\"queryStatus\":\"%08lx\",",
        header[1],header[4],header[2],header[3],static_cast<unsigned long>(status));
    if(ok) {
        // Fresh local output only. Never publish the vendor-private bytes.
        if(fopen_s(&file,argv[2],"wbx") || !file) ok=false;
        else { ok=std::fwrite(buffer.data(),1,buffer.size(),file)==buffer.size(); if(std::fclose(file)) ok=false; }
    }
    D3DKMT_CLOSEADAPTER close{}; close.hAdapter=adapter.hAdapter;
    status=D3DKMTCloseAdapter(&close); ok=ok && status>=0;
    std::printf("\"closeStatus\":\"%08lx\",\"success\":%s}\n",static_cast<unsigned long>(status),ok?"true":"false");
    return ok?0:1;
}
