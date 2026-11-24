// SPDX-License-Identifier: MIT
// Bounded D3D12 producer for the cross-process QEMU SDL relay control.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;
constexpr unsigned Width=3840,Height=2160;
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void check(HRESULT value,const char* message) { require(SUCCEEDED(value),message); }
struct Handle {
    HANDLE value=nullptr;
    void close() {
        if(value) { require(CloseHandle(value)!=FALSE,"Owned native handle did not close");value=nullptr; }
    }
    ~Handle() { if(value) CloseHandle(value); }
};
static std::string quote(const std::string& text) {
    std::ostringstream value;value<<'"';
    for(const unsigned char c:text) {
        if(c=='"' || c=='\\') value<<'\\'<<c;
        else if(c<32) value<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;
        else value<<c;
    }
    value<<'"';return value.str();
}
static std::string utf8(const wchar_t* text) {
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,nullptr,0,nullptr,nullptr);
    require(size>0,"Cannot encode NVIDIA adapter name");
    std::string result(static_cast<std::size_t>(size),'\0');
    require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,result.data(),size,nullptr,nullptr)==size,"Adapter name encoding failed");
    result.pop_back();return result;
}
static std::uint64_t budget(IDXGIAdapter1* adapter) {
    ComPtr<IDXGIAdapter3> query;check(adapter->QueryInterface(IID_PPV_ARGS(&query)),"Cannot query GPU usage");
    std::uint64_t bytes=0;
    for(const auto group:{DXGI_MEMORY_SEGMENT_GROUP_LOCAL,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};check(query->QueryVideoMemoryInfo(0,group,&info),"GPU usage query failed");
        require(info.CurrentUsage<=UINT64_MAX-bytes,"GPU usage overflow");bytes+=info.CurrentUsage;
    }
    require(bytes<=1073741824,"Producer exceeded its unchanged reported GPU usage guard");
    return bytes;
}
static void wait(ID3D12Fence* fence,std::uint64_t target,HANDLE event) {
    auto observed=fence->GetCompletedValue();require(observed!=UINT64_MAX,"GPU device lost");
    if(observed<target) {
        check(fence->SetEventOnCompletion(target,event),"Cannot register consumer completion");
        require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"QEMU GPU consumer did not retire within five seconds");
        observed=fence->GetCompletedValue();
    }
    require(observed!=UINT64_MAX && observed>=target,"Consumer fence target did not retire");
}
static void run(const wchar_t* session,const wchar_t* generationText,bool bgra) {
    require(std::wcslen(session)==32,"Session must contain exactly 32 lowercase hex digits");
    std::string sessionAscii;
    for(unsigned i=0;i<32;++i) {
        require((session[i]>=L'0' && session[i]<=L'9') || (session[i]>=L'a' && session[i]<=L'f'),"Invalid session hex");
        sessionAscii.push_back(static_cast<char>(session[i]));
    }
    require(*generationText>=L'1' && *generationText<=L'9',"Generation must be positive canonical decimal");
    std::uint64_t generation=0;
    for(const wchar_t* p=generationText;*p;++p) {
        require(*p>=L'0' && *p<=L'9' && generation<=(((1ull<<48)-1)-static_cast<unsigned>(*p-L'0'))/10,
                "Generation is invalid or out of bounds");
        generation=generation*10+static_cast<unsigned>(*p-L'0');
    }
    ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Cannot create DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
    for(unsigned index=0;index<32;++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const auto result=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));
        if(result==DXGI_ERROR_NOT_FOUND) break;
        check(result,"Cannot enumerate GPU adapters");DXGI_ADAPTER_DESC1 desc{};check(candidate->GetDesc1(&desc),"Cannot identify GPU");
        if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter=std::move(candidate);description=desc;break;
        }
    }
    require(adapter.Get()!=nullptr,"No hardware NVIDIA adapter");
    std::ostringstream luid;
    for(const unsigned char byte:std::array<unsigned char,8>{
        static_cast<unsigned char>(description.AdapterLuid.LowPart),
        static_cast<unsigned char>(description.AdapterLuid.LowPart>>8),
        static_cast<unsigned char>(description.AdapterLuid.LowPart>>16),
        static_cast<unsigned char>(description.AdapterLuid.LowPart>>24),
        static_cast<unsigned char>(description.AdapterLuid.HighPart),
        static_cast<unsigned char>(static_cast<unsigned>(description.AdapterLuid.HighPart)>>8),
        static_cast<unsigned char>(static_cast<unsigned>(description.AdapterLuid.HighPart)>>16),
        static_cast<unsigned char>(static_cast<unsigned>(description.AdapterLuid.HighPart)>>24)})
        luid<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(byte);
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Cannot create NVIDIA D3D12 device");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=Width;desc.Height=Height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"Cannot create named GPU texture");
    ComPtr<ID3D12Fence> producer,consumer;
    check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producer)),"Cannot create producer fence");
    check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&consumer)),"Cannot create consumer fence");
    std::wostringstream prefix;prefix<<L"Local\\TryOmarchy-SDL-"<<session<<L'-'<<std::hex<<std::setw(16)<<std::setfill(L'0')<<generation;
    const auto textureName=prefix.str()+L"-texture",producerName=prefix.str()+L"-producer",consumerName=prefix.str()+L"-consumer";
    Handle textureHandle,producerHandle,consumerHandle,event;
    check(device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,textureName.c_str(),&textureHandle.value),"Cannot export named GPU resource");
    check(device->CreateSharedHandle(producer.Get(),nullptr,GENERIC_ALL,producerName.c_str(),&producerHandle.value),"Cannot export producer fence");
    check(device->CreateSharedHandle(consumer.Get(),nullptr,GENERIC_ALL,consumerName.c_str(),&consumerHandle.value),"Cannot export consumer fence");
    for(const auto handle:{textureHandle.value,producerHandle.value,consumerHandle.value}) {
        DWORD flags=0;require(GetHandleInformation(handle,&flags)!=FALSE && !(flags&HANDLE_FLAG_INHERIT),"An exported NT handle is inheritable");
    }
    event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event.value!=nullptr,"Cannot create fence event");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"Cannot create producer GPU queue");
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Cannot create command allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Cannot create GPU clear commands");
    check(commands->Close(),"Cannot close the initial GPU commands");
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};rtvDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtvDesc.NumDescriptors=1;
    check(device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtv)),"Cannot create RTV");
    const auto view=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(texture.Get(),nullptr,view);
    const auto initialUsage=budget(adapter.Get());
    std::cout<<"{\"namedProducerReady\":true,\"session\":"<<quote(sessionAscii)<<",\"generation\":"<<generation
        <<",\"width\":"<<Width<<",\"height\":"<<Height<<",\"format\":\""<<(bgra?"BGRA8":"RGBA8")<<"\",\"vendorId\":"<<description.VendorId
        <<",\"adapter\":"<<quote(utf8(description.Description))<<",\"adapterLuid\":"<<quote(luid.str())
        <<",\"independentFences\":true,\"nonInheritableExports\":true,\"gpuUsageBytes\":"<<initialUsage<<"}\n";
    bool exportsClosed=false;unsigned frames=0;
    const std::array<std::array<unsigned char,4>,4> colors{{{32,64,128,255},{192,32,64,255},{64,192,32,255},{128,64,192,255}}};
    for(;;) {
        char line[64]{};std::cin.getline(line,sizeof line);require(std::cin.good(),"Owner command disconnected or exceeded its bound");
        const std::string instruction(line);
        if(instruction=="close-handles") {
            require(!exportsClosed && !frames,"Original handles may close only once, before frames");
            textureHandle.close();producerHandle.close();consumerHandle.close();exportsClosed=true;
            std::cout<<"{\"namedProducerExportsClosed\":true,\"handles\":3}\n";continue;
        }
        if(instruction=="close") {
            require(exportsClosed && frames>=2,"Cannot accept an incomplete producer control");
            break;
        }
        require(exportsClosed && frames<300 && instruction=="frame:"+std::to_string(frames+1),"Stale/out-of-order producer command");
        const unsigned frame=frames+1;
        if(frames) wait(consumer.Get(),frames,event.value);
        check(allocator->Reset(),"Cannot reset the retired allocator");check(commands->Reset(allocator.Get(),nullptr),"Cannot reset retired GPU commands");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=texture.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1,&barrier);
        for(unsigned quad=0;quad<4;++quad) {
            const D3D12_RECT rect{static_cast<LONG>((quad%2)*Width/2),static_cast<LONG>((quad/2)*Height/2),
                static_cast<LONG>((quad%2+1)*Width/2),static_cast<LONG>((quad/2+1)*Height/2)};
            float color[4];for(unsigned channel=0;channel<4;++channel)color[channel]=static_cast<float>(colors[(quad+frame-1)%4][channel])/255.0f;
            commands->ClearRenderTargetView(view,color,1,&rect);
        }
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);
        check(commands->Close(),"Cannot close GPU clear commands");ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(producer.Get(),frame),"Cannot queue producer completion");
        std::cout<<"{\"namedProducerFrameQueued\":true,\"frame\":"<<frame<<",\"producerTarget\":"<<frame<<",\"cpuTextureUpload\":false}\n";
        wait(consumer.Get(),frame,event.value);
        check(device->GetDeviceRemovedReason(),"NVIDIA device removed");
        const auto usage=budget(adapter.Get());
        std::cout<<"{\"namedProducerConsumerRetired\":true,\"frame\":"<<frame<<",\"producerFence\":"<<producer->GetCompletedValue()
            <<",\"consumerFence\":"<<consumer->GetCompletedValue()<<",\"gpuUsageBytes\":"<<usage<<"}\n";
        frames=frame;
    }
    commands.Reset();allocator.Reset();queue.Reset();rtv.Reset();texture.Reset();producer.Reset();consumer.Reset();event.close();
    check(device->GetDeviceRemovedReason(),"Device lost before cleanup");device.Reset();adapter.Reset();factory.Reset();
    std::cout<<"{\"namedProducerComplete\":true,\"frames\":"<<frames
        <<",\"exportsClosed\":3,\"gpuObjectsReleased\":true,\"guestFrameVerified\":false,\"nearNativePerformanceVerified\":false}\n";
}
int wmain(int argc,wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
        const bool bgra=argc==4 && !std::wcscmp(argv[3],L"--bgra");
        require(argc==3 || bgra,"Usage: d3d12-named-producer SESSION32 GENERATION [--bgra]");
        run(argv[1],argv[2],bgra);return 0;
    } catch(const std::exception& error) {
        std::cerr<<"{\"namedProducerRejected\":true,\"error\":"<<quote(error.what())<<"}\n";return 1;
    }
}
