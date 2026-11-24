// SPDX-License-Identifier: MIT
// Standalone host interop control. QEMU/guest/frontend acceptance is separate.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <gl/GL.h>
#include "gl_external_api.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" { __declspec(dllexport) DWORD NvOptimusEnablement = 1; }
using Microsoft::WRL::ComPtr;
constexpr unsigned Width=3840,Height=2160;
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void check(HRESULT value,const char* message) { require(SUCCEEDED(value),message); }
static void glCheck(const char* stage) {
    const auto error=glGetError();
    if(error!=GL_NO_ERROR) {
        std::cerr<<"{\"glFailure\":true,\"stage\":\""<<stage<<"\",\"error\":"<<error<<"}\n";
        throw std::runtime_error(stage);
    }
}
static std::string quote(const char* text) {
    std::ostringstream value;value<<'"';
    for(const unsigned char c:std::string(text?text:"")) {
        if(c=='"' || c=='\\') value<<'\\'<<c;
        else if(c<32) value<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;
        else value<<c;
    }
    value<<'"';return value.str();
}
template<class Function,class Pointer> static Function function(Pointer address) {
    static_assert(sizeof(Function)==sizeof(address),"Native function pointer size");
    require(address!=nullptr,"Missing required API entry point");
    Function result;std::memcpy(&result,&address,sizeof result);return result;
}
struct SDL_Window;
struct ModuleReference {
    HMODULE& value;
    ~ModuleReference() { if(value) FreeLibrary(value); }
};
struct Sdl {
    HMODULE module=nullptr;ModuleReference moduleOwner{module};
    SDL_Window* window=nullptr;void* context=nullptr;void* importContext=nullptr;bool started=false;
    Sdl(const Sdl&)=delete;
    Sdl& operator=(const Sdl&)=delete;
    int (__cdecl *init)(unsigned)=nullptr;
    void (__cdecl *quit)()=nullptr;
    const char* (__cdecl *error)()=nullptr;
    int (__cdecl *hint)(const char*,const char*)=nullptr;
    SDL_Window* (__cdecl *create)(const char*,int,int,int,int,unsigned)=nullptr;
    void (__cdecl *destroy)(SDL_Window*)=nullptr;
    void* (__cdecl *createContext)(SDL_Window*)=nullptr;
    void (__cdecl *deleteContext)(void*)=nullptr;
    int (__cdecl *makeCurrent)(SDL_Window*,void*)=nullptr;
    int (__cdecl *setAttribute)(int,int)=nullptr;
    void* (__cdecl *getProc)(const char*)=nullptr;
    void (__cdecl *swap)(SDL_Window*)=nullptr;
    int (__cdecl *swapInterval)(int)=nullptr;
    void (__cdecl *drawableSize)(SDL_Window*,int*,int*)=nullptr;
    template<class T> T symbol(const char* name) { return function<T>(GetProcAddress(module,name)); }
    explicit Sdl(const wchar_t* path) {
        require(std::filesystem::path(path).is_absolute(),"SDL DLL path must be absolute");
        module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module!=nullptr,"Cannot load the pinned SDL library");
        init=symbol<decltype(init)>("SDL_Init");quit=symbol<decltype(quit)>("SDL_Quit");
        error=symbol<decltype(error)>("SDL_GetError");hint=symbol<decltype(hint)>("SDL_SetHint");
        create=symbol<decltype(create)>("SDL_CreateWindow");destroy=symbol<decltype(destroy)>("SDL_DestroyWindow");
        createContext=symbol<decltype(createContext)>("SDL_GL_CreateContext");
        deleteContext=symbol<decltype(deleteContext)>("SDL_GL_DeleteContext");
        makeCurrent=symbol<decltype(makeCurrent)>("SDL_GL_MakeCurrent");getProc=symbol<decltype(getProc)>("SDL_GL_GetProcAddress");
        setAttribute=symbol<decltype(setAttribute)>("SDL_GL_SetAttribute");
        swap=symbol<decltype(swap)>("SDL_GL_SwapWindow");swapInterval=symbol<decltype(swapInterval)>("SDL_GL_SetSwapInterval");
        drawableSize=symbol<decltype(drawableSize)>("SDL_GL_GetDrawableSize");
    }
    void open() {
        // Match the official SDL root window's default GL context and DPI hint.
        // No keyboard, cursor, input-grab or event-routing hints are changed.
        hint("SDL_WINDOWS_DPI_AWARENESS","permonitorv2");
        if(init(0x20u)) { std::cerr<<quote(error())<<'\n';throw std::runtime_error("SDL video initialization failed"); }
        started=true;
        window=create("Try Omarchy D3D12/SDL interop control",0x1fff0000,0x1fff0000,1280,720,0x2u|0x8u);
        require(window!=nullptr,"Cannot create the hidden SDL OpenGL window");
        context=createContext(window);
        if(!context) { std::cerr<<quote(error())<<'\n';throw std::runtime_error("Cannot create SDL OpenGL context"); }
        require(makeCurrent(window,context)==0,"Cannot activate SDL OpenGL context");
        require(swapInterval(0)==0,"Cannot set the SDL swap interval");
    }
    void openSharedImportContext() {
        // SDL_GLattr values from the pinned SDL 2.32.10 ABI. Mirror QEMU's
        // shared core-context creation while leaving its root context intact.
        require(makeCurrent(window,context)==0,"Cannot activate the SDL root context");
        require(setAttribute(22,1)==0 && setAttribute(21,1)==0 &&
                setAttribute(17,4)==0 && setAttribute(18,5)==0,"Cannot configure the shared SDL context");
        importContext=createContext(window);
        require(importContext!=nullptr && importContext!=context,"Cannot create a distinct shared import context");
        require(makeCurrent(window,importContext)==0,"Cannot activate the shared import context");
    }
    void activateRoot() {
        require(makeCurrent(window,context)==0,"Cannot restore the SDL root context");
    }
    void close() {
        if(importContext) { deleteContext(importContext);importContext=nullptr; }
        if(context) { deleteContext(context);context=nullptr; }
        if(window) { destroy(window);window=nullptr; }
        if(started) { quit();started=false; }
        if(module) { require(FreeLibrary(module)!=FALSE,"SDL module reference not released");module=nullptr; }
    }
    ~Sdl() {
        if(importContext && deleteContext) deleteContext(importContext);
        if(context && deleteContext) deleteContext(context);
        if(window && destroy) destroy(window);
        if(started && quit) quit();
    }
    template<class T> T gl(const char* name) { return function<T>(getProc(name)); }
};
struct Gl {
    PFNGLGETSTRINGIPROC stringi;
    PFNGLGETUNSIGNEDBYTEVEXTPROC unsignedBytes;
    PFNGLCREATEMEMORYOBJECTSEXTPROC createMemory;
    PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemory;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryParameter;
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC importMemory;
    PFNGLIMPORTMEMORYWIN32NAMEEXTPROC importMemoryName;
    PFNGLTEXSTORAGEMEM2DEXTPROC textureStorage;
    PFNGLGENSEMAPHORESEXTPROC genSemaphores;
    PFNGLDELETESEMAPHORESEXTPROC deleteSemaphores;
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC importSemaphore;
    PFNGLIMPORTSEMAPHOREWIN32NAMEEXTPROC importSemaphoreName;
    PFNGLSEMAPHOREPARAMETERUI64VEXTPROC semaphoreParameter;
    PFNGLWAITSEMAPHOREEXTPROC waitSemaphore;
    PFNGLSIGNALSEMAPHOREEXTPROC signalSemaphore;
    PFNGLGENFRAMEBUFFERSPROC genFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC deleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC bindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC attachTexture;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC framebufferStatus;
    PFNGLBLITFRAMEBUFFERPROC blit;
    explicit Gl(Sdl& s):
        stringi(s.gl<decltype(stringi)>("glGetStringi")),
        unsignedBytes(s.gl<decltype(unsignedBytes)>("glGetUnsignedBytevEXT")),
        createMemory(s.gl<decltype(createMemory)>("glCreateMemoryObjectsEXT")),
        deleteMemory(s.gl<decltype(deleteMemory)>("glDeleteMemoryObjectsEXT")),
        memoryParameter(s.gl<decltype(memoryParameter)>("glMemoryObjectParameterivEXT")),
        importMemory(s.gl<decltype(importMemory)>("glImportMemoryWin32HandleEXT")),
        importMemoryName(s.gl<decltype(importMemoryName)>("glImportMemoryWin32NameEXT")),
        textureStorage(s.gl<decltype(textureStorage)>("glTexStorageMem2DEXT")),
        genSemaphores(s.gl<decltype(genSemaphores)>("glGenSemaphoresEXT")),
        deleteSemaphores(s.gl<decltype(deleteSemaphores)>("glDeleteSemaphoresEXT")),
        importSemaphore(s.gl<decltype(importSemaphore)>("glImportSemaphoreWin32HandleEXT")),
        importSemaphoreName(s.gl<decltype(importSemaphoreName)>("glImportSemaphoreWin32NameEXT")),
        semaphoreParameter(s.gl<decltype(semaphoreParameter)>("glSemaphoreParameterui64vEXT")),
        waitSemaphore(s.gl<decltype(waitSemaphore)>("glWaitSemaphoreEXT")),
        signalSemaphore(s.gl<decltype(signalSemaphore)>("glSignalSemaphoreEXT")),
        genFramebuffers(s.gl<decltype(genFramebuffers)>("glGenFramebuffers")),
        deleteFramebuffers(s.gl<decltype(deleteFramebuffers)>("glDeleteFramebuffers")),
        bindFramebuffer(s.gl<decltype(bindFramebuffer)>("glBindFramebuffer")),
        attachTexture(s.gl<decltype(attachTexture)>("glFramebufferTexture2D")),
        framebufferStatus(s.gl<decltype(framebufferStatus)>("glCheckFramebufferStatus")),
        blit(s.gl<decltype(blit)>("glBlitFramebuffer")) {}
    bool extension(const char* name) const {
        GLint count=0;glGetIntegerv(GL_NUM_EXTENSIONS,&count);glCheck("extension count");
        require(count>0 && count<65536,"Invalid OpenGL extension count");
        for(GLint index=0;index<count;++index) {
            const auto value=stringi(GL_EXTENSIONS,static_cast<GLuint>(index));
            if(value && !std::strcmp(reinterpret_cast<const char*>(value),name)) return true;
        }
        return false;
    }
};
struct Handle {
    HANDLE value=nullptr;
    void close() { if(value) { require(CloseHandle(value)!=FALSE,"Owned handle not closed");value=nullptr; } }
    ~Handle() { if(value) CloseHandle(value); }
};
static std::uint64_t budget(IDXGIAdapter1* adapter) {
    ComPtr<IDXGIAdapter3> query;check(adapter->QueryInterface(IID_PPV_ARGS(&query)),"Cannot query GPU budget");
    std::uint64_t total=0;
    for(const auto group:{DXGI_MEMORY_SEGMENT_GROUP_LOCAL,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};check(query->QueryVideoMemoryInfo(0,group,&info),"GPU usage query failed");
        require(info.CurrentUsage<=UINT64_MAX-total,"GPU usage overflow");total+=info.CurrentUsage;
    }
    require(total<=1073741824,"Owned probe exceeds the reported GPU usage guard");
    return total;
}
static void wait(ID3D12Fence* fence,std::uint64_t target,HANDLE event) {
    auto observed=fence->GetCompletedValue();require(observed!=UINT64_MAX,"GPU device lost");
    if(observed<target) {
        check(fence->SetEventOnCompletion(target,event),"Cannot register shared fence completion");
        require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"Shared fence did not retire");
        observed=fence->GetCompletedValue();
    }
    require(observed!=UINT64_MAX && observed>=target,"Shared fence did not reach its target");
}
using Color=std::array<unsigned char,4>;
static std::array<Color,4> palette(unsigned frame) {
    const std::array<Color,4> base{{{32,64,128,255},{192,32,64,255},{64,192,32,255},{128,64,192,255}}};
    std::array<Color,4> result{};
    for(unsigned q=0;q<4;++q) result[q]=base[(q+frame-1)%4];
    return result;
}
static std::uint64_t verify(const std::vector<unsigned char>& pixels,unsigned width,unsigned height,unsigned frame) {
    require(pixels.size()==static_cast<std::size_t>(width)*height*4,"Incomplete GPU pixels");
    const auto colors=palette(frame);std::uint64_t digest=14695981039346656037ull;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        const auto& color=colors[(y>=height/2?2u:0u)+(x>=width/2?1u:0u)];
        for(unsigned channel=0;channel<4;++channel) {
            const auto byte=pixels[(static_cast<std::size_t>(y)*width+x)*4+channel];
            require(std::abs(static_cast<int>(byte)-color[channel])<=1,"GPU quadrant pixels differ from the independent clear pattern");
            digest^=byte;digest*=1099511628211ull;
        }
    }
    return digest;
}
static void save(const std::filesystem::path& directory,const wchar_t* prefix,unsigned frame,const std::vector<unsigned char>& bytes) {
    const auto name=std::wstring(prefix)+std::to_wstring(frame)+L".rgba";
    std::ofstream out(directory/name,std::ios::binary);require(out.is_open(),"Cannot save diagnostic GPU pixels");
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    out.close();require(!out.fail(),"Incomplete diagnostic pixel write");
}
static void run(const wchar_t* dll,const std::filesystem::path& directory,bool capabilitiesOnly,bool sharedContexts) {
    Sdl s(dll);s.open();
    const auto vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
    require(vendor && renderer && version && std::strstr(vendor,"NVIDIA"),"OpenGL context is not on NVIDIA");
    std::cout<<"{\"sdlGlContext\":true,\"vendor\":"<<quote(vendor)<<",\"renderer\":"<<quote(renderer)<<",\"version\":"<<quote(version)<<"}\n";
    if(sharedContexts) {
        s.openSharedImportContext();
        const auto importVendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const auto importRenderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto importVersion=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        require(importVendor && importRenderer && importVersion &&
                !std::strcmp(vendor,importVendor) && !std::strcmp(renderer,importRenderer),
                "Shared import context uses a different GPU");
        std::cout<<"{\"sdlGlSharedImportContext\":true,\"distinctContexts\":true,\"vendor\":"<<quote(importVendor)
            <<",\"renderer\":"<<quote(importRenderer)<<",\"version\":"<<quote(importVersion)<<"}\n";
    }
    Gl g(s);
    for(const auto name:{"GL_EXT_memory_object","GL_EXT_memory_object_win32","GL_EXT_semaphore","GL_EXT_semaphore_win32"})
        require(g.extension(name),"Required external-memory/fence extension is not advertised");
    std::array<unsigned char,8> luid{};g.unsignedBytes(GL_DEVICE_LUID_EXT,luid.data());glCheck("OpenGL adapter LUID");
    ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Cannot create DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
    for(unsigned index=0;index<32;++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const auto result=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));
        if(result==DXGI_ERROR_NOT_FOUND) break;
        check(result,"Cannot enumerate DXGI adapter");DXGI_ADAPTER_DESC1 desc{};check(candidate->GetDesc1(&desc),"Cannot describe DXGI adapter");
        if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && !std::memcmp(&desc.AdapterLuid,luid.data(),luid.size())) {
            adapter=std::move(candidate);description=desc;break;
        }
    }
    require(adapter.Get()!=nullptr,"D3D12 and OpenGL adapter LUIDs differ");
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Cannot create NVIDIA D3D12 device");
    const auto before=budget(adapter.Get());
    std::cout<<"{\"sdlGlExternalCapabilities\":true,\"vendor\":"<<quote(vendor)<<",\"renderer\":"<<quote(renderer)
        <<",\"version\":"<<quote(version)<<",\"sameAdapterLuid\":true,\"vendorId\":"<<description.VendorId
        <<",\"gpuUsageBytes\":"<<before<<",\"windowHidden\":true,\"qemuFrontendVerified\":false}\n";
    if(capabilitiesOnly) { device.Reset();adapter.Reset();factory.Reset();s.close();return; }
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=Width;desc.Height=Height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"Cannot create shared D3D12 texture");
    ComPtr<ID3D12Fence> producerFence,consumerFence;
    check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producerFence)),"Cannot create producer fence");
    check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&consumerFence)),"Cannot create consumer fence");
    std::array<unsigned char,16> random{};
    require(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Cannot generate shared-object session");
    std::wostringstream prefix;prefix<<L"Local\\TryOmarchy-SDL-";
    for(const auto byte:random) prefix<<std::hex<<std::setw(2)<<std::setfill(L'0')<<static_cast<unsigned>(byte);
    const auto memoryName=prefix.str()+L"-texture",producerName=prefix.str()+L"-producer",consumerName=prefix.str()+L"-consumer";
    Handle memoryHandle,producerHandle,consumerHandle,event;
    event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event.value!=nullptr,"Cannot create fence event");
    check(device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,memoryName.c_str(),&memoryHandle.value),"Cannot export named D3D12 resource");
    check(device->CreateSharedHandle(producerFence.Get(),nullptr,GENERIC_ALL,producerName.c_str(),&producerHandle.value),"Cannot export named producer fence");
    check(device->CreateSharedHandle(consumerFence.Get(),nullptr,GENERIC_ALL,consumerName.c_str(),&consumerHandle.value),"Cannot export named consumer fence");
    for(const auto handle:{memoryHandle.value,producerHandle.value,consumerHandle.value}) {
        DWORD flags=0;require(GetHandleInformation(handle,&flags)!=FALSE && !(flags&HANDLE_FLAG_INHERIT),"An exported handle is inheritable");
    }
    GLuint memory=0,sharedTexture=0,producerSemaphore=0,consumerSemaphore=0,framebuffer=0;
    g.createMemory(1,&memory);const GLint dedicated=GL_TRUE;g.memoryParameter(memory,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    // EXT_external_objects_win32 ignores size for a D3D12 resource. Zero is the
    // specified compatibility value; this is not an unbounded opaque allocation.
    g.importMemoryName(memory,0,GL_HANDLE_TYPE_D3D12_RESOURCE_EXT,memoryName.c_str());glCheck("Named D3D12 resource import");
    glGenTextures(1,&sharedTexture);glBindTexture(GL_TEXTURE_2D,sharedTexture);
    g.textureStorage(GL_TEXTURE_2D,1,GL_RGBA8,Width,Height,memory,0);glCheck("External texture storage");
    g.genSemaphores(1,&producerSemaphore);g.genSemaphores(1,&consumerSemaphore);
    g.importSemaphoreName(producerSemaphore,GL_HANDLE_TYPE_D3D12_FENCE_EXT,producerName.c_str());
    g.importSemaphoreName(consumerSemaphore,GL_HANDLE_TYPE_D3D12_FENCE_EXT,consumerName.c_str());glCheck("Named D3D12 fence imports");
    memoryHandle.close();producerHandle.close();consumerHandle.close();
    if(sharedContexts) {
        // The texture, memory and semaphores are shareable. FBOs are not:
        // create and consume the scanout FBO only in the SDL root context.
        glFlush();glCheck("Publish imported GL objects to the shared root");
        s.activateRoot();
        std::array<unsigned char,8> rootLuid{};g.unsignedBytes(GL_DEVICE_LUID_EXT,rootLuid.data());
        require(rootLuid==luid,"SDL root and import contexts have different adapter LUIDs");
        glCheck("Root adapter LUID after shared import");
    }
    g.genFramebuffers(1,&framebuffer);g.bindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);
    g.attachTexture(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,sharedTexture,0);
    require(g.framebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Imported texture framebuffer is incomplete");glCheck("Imported framebuffer");
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};rtvDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtvDesc.NumDescriptors=1;
    check(device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtv)),"Cannot create RTV heap");
    const auto view=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(texture.Get(),nullptr,view);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"Cannot create producer queue");
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Cannot create allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Cannot create clear command list");
    check(commands->Close(),"Cannot close initial command list");
    int drawableWidth=0,drawableHeight=0;s.drawableSize(s.window,&drawableWidth,&drawableHeight);
    require(drawableWidth>0 && drawableHeight>0 && drawableWidth<=4096 && drawableHeight<=4096,"Invalid SDL drawable extent");
    std::cout<<"{\"sdlD3D12ObjectsOpened\":true,\"sourceWidth\":"<<Width<<",\"sourceHeight\":"<<Height
        <<",\"windowWidth\":"<<drawableWidth<<",\"windowHeight\":"<<drawableHeight<<",\"originalNtHandlesClosed\":true,"
        "\"namedObjects\":true,\"independentFences\":true,\"sharedImportContext\":"<<(sharedContexts?"true":"false")<<"}\n";
    const GLenum layout=GL_LAYOUT_GENERAL_EXT;
    for(unsigned frame=1;frame<=2;++frame) {
        check(allocator->Reset(),"Cannot reset retired allocator");check(commands->Reset(allocator.Get(),nullptr),"Cannot reset retired command list");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=texture.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1,&barrier);
        const auto colors=palette(frame);
        for(unsigned quad=0;quad<4;++quad) {
            const D3D12_RECT rect{static_cast<LONG>((quad%2)*Width/2),static_cast<LONG>((quad/2)*Height/2),
                static_cast<LONG>((quad%2+1)*Width/2),static_cast<LONG>((quad/2+1)*Height/2)};
            float color[4];for(unsigned c=0;c<4;++c) color[c]=static_cast<float>(colors[quad][c])/255.0f;
            commands->ClearRenderTargetView(view,color,1,&rect);
        }
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);
        check(commands->Close(),"Cannot close producer commands");ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
        const GLuint64 produced=frame,returned=frame;check(queue->Signal(producerFence.Get(),produced),"Cannot enqueue producer fence");
        g.semaphoreParameter(producerSemaphore,GL_D3D12_FENCE_VALUE_EXT,&produced);
        g.waitSemaphore(producerSemaphore,0,nullptr,1,&sharedTexture,&layout);glCheck("OpenGL GPU wait on D3D12 producer");
        g.bindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);glReadBuffer(GL_COLOR_ATTACHMENT0);
        std::vector<unsigned char> raw(static_cast<std::size_t>(Width)*Height*4);
        glReadPixels(0,0,Width,Height,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());glCheck("Full imported GPU texture readback");
        save(directory,L"interop-source-",frame,raw);const auto sourceDigest=verify(raw,Width,Height,frame);
        g.bindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
        g.blit(0,0,Width,Height,0,0,drawableWidth,drawableHeight,GL_COLOR_BUFFER_BIT,GL_NEAREST);glCheck("GPU blit into SDL backbuffer");
        g.bindFramebuffer(GL_READ_FRAMEBUFFER,0);glReadBuffer(GL_BACK);
        std::vector<unsigned char> output(static_cast<std::size_t>(drawableWidth)*drawableHeight*4);
        glReadPixels(0,0,drawableWidth,drawableHeight,GL_RGBA,GL_UNSIGNED_BYTE,output.data());glCheck("SDL backbuffer readback");
        save(directory,L"interop-window-",frame,output);
        const auto outputDigest=verify(output,static_cast<unsigned>(drawableWidth),static_cast<unsigned>(drawableHeight),frame);
        s.swap(s.window);glCheck("SDL swap");
        g.semaphoreParameter(consumerSemaphore,GL_D3D12_FENCE_VALUE_EXT,&returned);
        g.signalSemaphore(consumerSemaphore,0,nullptr,1,&sharedTexture,&layout);glFlush();glCheck("OpenGL GPU signal to D3D12");
        wait(consumerFence.Get(),returned,event.value);check(device->GetDeviceRemovedReason(),"D3D12 device failed");
        const auto usage=budget(adapter.Get());
        std::cout<<"{\"sdlD3D12InteropFrame\":true,\"frame\":"<<frame<<",\"sourceWidth\":"<<Width<<",\"sourceHeight\":"<<Height
            <<",\"windowWidth\":"<<drawableWidth<<",\"windowHeight\":"<<drawableHeight<<",\"producerFence\":"<<produced
            <<",\"consumerFence\":"<<returned<<",\"observedProducerFence\":"<<producerFence->GetCompletedValue()
            <<",\"observedConsumerFence\":"<<consumerFence->GetCompletedValue()<<",\"independentFences\":true,\"sourceFnv1a\":\""
            <<std::hex<<sourceDigest<<"\",\"windowFnv1a\":\""<<outputDigest<<std::dec<<"\",\"gpuUsageBytes\":"<<usage
            <<",\"cpuUpload\":false,\"diagnosticReadbacks\":2,\"displayVerified\":false}\n";
    }
    glFinish();glCheck("Final GL retirement");
    g.bindFramebuffer(GL_FRAMEBUFFER,0);g.deleteFramebuffers(1,&framebuffer);glDeleteTextures(1,&sharedTexture);
    g.deleteMemory(1,&memory);g.deleteSemaphores(1,&producerSemaphore);g.deleteSemaphores(1,&consumerSemaphore);glCheck("GL shared objects released");
    commands.Reset();allocator.Reset();queue.Reset();rtv.Reset();texture.Reset();producerFence.Reset();consumerFence.Reset();event.close();
    check(device->GetDeviceRemovedReason(),"Device removed before cleanup");device.Reset();adapter.Reset();factory.Reset();s.close();
    std::cout<<"{\"sdlD3D12InteropComplete\":true,\"frames\":2,\"sharedHandlesClosed\":3,\"sameAdapterLuid\":true,"
        "\"namedObjects\":true,\"independentFences\":true,"
        "\"gpuOnlyTransfer\":true,\"glObjectsReleased\":true,\"d3dObjectsReleased\":true,\"sdlClosed\":true,"
        "\"qemuFrontendVerified\":false,\"guestFrameVerified\":false,\"nearNativePerformanceVerified\":false,"
        "\"sharedImportContext\":"<<(sharedContexts?"true":"false")<<"}\n";
}
int wmain(int argc,wchar_t** argv) {
    try {
        const bool capabilitiesOnly=argc==4 && !std::wcscmp(argv[3],L"--capabilities-only");
        const bool sharedContexts=argc==4 && !std::wcscmp(argv[3],L"--shared-contexts");
        require(argc==3 || capabilitiesOnly || sharedContexts,"Usage: probe SDL2.dll fresh-output-directory [--capabilities-only|--shared-contexts]");
        const std::filesystem::path directory(argv[2]);require(directory.is_absolute(),"Output directory must be absolute");
        require(CreateDirectoryW(directory.c_str(),nullptr)!=FALSE,"Use a fresh output directory");
        std::cout.setf(std::ios::unitbuf);run(argv[1],directory,capabilitiesOnly,sharedContexts);return 0;
    } catch(const std::exception& error) {
        std::cerr<<"{\"sdlD3D12InteropRejected\":true,\"error\":"<<quote(error.what())<<"}\n";return 1;
    }
}
