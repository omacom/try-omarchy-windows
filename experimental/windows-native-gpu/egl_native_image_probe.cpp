// SPDX-License-Identifier: MIT
// Real EGL DMA-BUF image import/export against the current NVIDIA Mesa context.
// This is a buffer/API diagnostic, not a desktop or scanout acceptance test.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>
struct HGLRC__;
using HGLRC = HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include "drm-uapi/drm_fourcc.h"
#include "guest-kernel/wddm_aperture.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Microsoft::WRL::ComPtr;
static wddm_native_buffer_stats imageBaseline{};
static bool imageEnabled=false;

extern "C" int wddm_native_image_test(EGLDisplay display, EGLContext context) {
#ifndef WDDM_BRIDGE_NATIVE_IMAGE_WIDTH
#define WDDM_BRIDGE_NATIVE_IMAGE_WIDTH 3840
#define WDDM_BRIDGE_NATIVE_IMAGE_HEIGHT 2160
#endif
    constexpr unsigned Width=WDDM_BRIDGE_NATIVE_IMAGE_WIDTH,Height=WDDM_BRIDGE_NATIVE_IMAGE_HEIGHT,ShaderWidth=130,ShaderHeight=73,BandRows=512;
#if defined(WDDM_BRIDGE_NATIVE_IMAGE_BGRX)
    constexpr auto Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    constexpr EGLint Fourcc=DRM_FORMAT_XRGB8888;
#elif defined(WDDM_BRIDGE_NATIVE_IMAGE_BGRA)
    constexpr auto Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    constexpr EGLint Fourcc=DRM_FORMAT_ARGB8888;
#elif defined(WDDM_BRIDGE_NATIVE_IMAGE_RGBX)
    constexpr auto Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr EGLint Fourcc=DRM_FORMAT_XBGR8888;
#else
    constexpr auto Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr EGLint Fourcc=DRM_FORMAT_ABGR8888;
#endif
    const auto failed=[](const char* stage) {
        std::printf("EGL_NATIVE_IMAGE_FAILURE stage=%s eglError=%04x errno=%d\n",stage,eglGetError(),errno); return 1;
    };
    const auto supported=(int(*)(void))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_supported");
    const char* extensions=eglQueryString(display,EGL_EXTENSIONS);
    if(!supported || !extensions) return failed("Hooks");
    const bool hasImport=std::strstr(extensions,"EGL_EXT_image_dma_buf_import ")!=nullptr;
    const bool hasExport=std::strstr(extensions,"EGL_MESA_image_dma_buf_export ")!=nullptr;
#ifdef WDDM_BRIDGE_NATIVE_IMAGE_DISABLED
    if(supported()!=0 || hasImport || hasExport) return failed("DisabledCapabilities");
    std::puts("EGL_NATIVE_IMAGE_DISABLED kernelEnabled=false importAbsent=true exportAbsent=true desktop=false");
    return 0;
#endif
    if(supported()!=1 || !hasImport || !hasExport) return failed("Capabilities");
    std::printf("EGL_NATIVE_IMAGE_BEGIN width=%u height=%u dxgi=%u fourcc=%08x implicitOpaque=true desktop=false\n",Width,Height,unsigned(Format),unsigned(Fourcc));
#define LOAD(type,name) const auto name=(type)eglGetProcAddress(#name); if(!name) return failed(#name)
    LOAD(PFNEGLCREATEIMAGEKHRPROC,eglCreateImageKHR);
    LOAD(PFNEGLDESTROYIMAGEKHRPROC,eglDestroyImageKHR);
    LOAD(PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC,eglExportDMABUFImageQueryMESA);
    LOAD(PFNEGLEXPORTDMABUFIMAGEMESAPROC,eglExportDMABUFImageMESA);
    LOAD(PFNEGLQUERYDMABUFFORMATSEXTPROC,eglQueryDmaBufFormatsEXT);
    LOAD(PFNEGLQUERYDMABUFMODIFIERSEXTPROC,eglQueryDmaBufModifiersEXT);
    LOAD(PFNGLGENTEXTURESPROC,glGenTextures);
    LOAD(PFNGLBINDTEXTUREPROC,glBindTexture);
    LOAD(PFNGLDELETETEXTURESPROC,glDeleteTextures);
    using ImageTarget=void(*)(GLenum,void*);
    LOAD(ImageTarget,glEGLImageTargetTexture2DOES);
    LOAD(PFNGLGENFRAMEBUFFERSPROC,glGenFramebuffers);
    LOAD(PFNGLBINDFRAMEBUFFERPROC,glBindFramebuffer);
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC,glFramebufferTexture2D);
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC,glCheckFramebufferStatus);
    LOAD(PFNGLDELETEFRAMEBUFFERSPROC,glDeleteFramebuffers);
    LOAD(PFNGLCLEARCOLORPROC,glClearColor);
    LOAD(PFNGLCLEARPROC,glClear);
    LOAD(PFNGLFINISHPROC,glFinish);
    LOAD(PFNGLREADPIXELSPROC,glReadPixels);
    LOAD(PFNGLPIXELSTOREIPROC,glPixelStorei);
    LOAD(PFNGLGETERRORPROC,glGetError);
    LOAD(PFNGLVIEWPORTPROC,glViewport);
    LOAD(PFNGLDRAWARRAYSPROC,glDrawArrays);
#undef LOAD
    EGLint count=0;
    if(!eglQueryDmaBufFormatsEXT(display,0,nullptr,&count) || count!=4) return failed("FormatCount");
    std::vector<EGLint> formats(count);
    if(!eglQueryDmaBufFormatsEXT(display,count,formats.data(),&count) || count!=4) return failed("Formats");
    for(EGLint wanted:{DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888}) {
        unsigned matches=0; for(auto f:formats) if(f==wanted) ++matches;
        EGLint modifiers=-1;
        if(matches!=1 || !eglQueryDmaBufModifiersEXT(display,wanted,0,nullptr,nullptr,&modifiers) || modifiers!=0)
            return failed("ImplicitFormats");
    }
    EGLint modifiers=0; eglGetError();
    if(eglQueryDmaBufModifiersEXT(display,DRM_FORMAT_RGB565,0,nullptr,nullptr,&modifiers) || eglGetError()!=EGL_BAD_PARAMETER)
        return failed("UnsupportedFormatQuery");
    std::puts("EGL_NATIVE_IMAGE_FORMATS rgba=true rgbx=true bgra=true bgrx=true explicitModifiers=0 linearAdvertised=false");
    const auto wrap=(int(*)(int,std::uint64_t))dlsym(RTLD_DEFAULT,"wddm_bridge_export_native_buffer");
    const auto stats=(int(*)(wddm_native_buffer_stats*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_stats");
    const auto info=(int(*)(int,std::uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info");
    const auto prepare=(int(*)(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t))dlsym(RTLD_DEFAULT,"wddm_bridge_prepare_native_resource");
    const auto cancel=(int(*)(void))dlsym(RTLD_DEFAULT,"wddm_bridge_cancel_native_resource");
    wddm_native_buffer_stats baseline{};
    if(!wrap || !stats || !info || !prepare || !cancel || stats(&baseline) || baseline.held || baseline.bytes || baseline.created!=baseline.released)
        return failed("Baseline");
    imageBaseline=baseline; imageEnabled=true;
    d3d12_interop_device_info native{}; mesa_glinterop_device_info device{};
    device.version=2; device.driver_data_size=sizeof native; device.driver_data=&native;
    if(MesaGLInteropEGLQueryDeviceInfo(display,context,&device)!=MESA_GLINTEROP_SUCCESS ||
       device.driver_data_size!=sizeof native || !native.device || !native.queue)
        return failed("NativeDevice");
    for(auto invalid:std::array<std::array<std::uint32_t,4>,5>{{{0,Height,Format,1},{Width,0,Format,1},{4097,Height,Format,1},{Width,Height,29,1},{Width,Height,Format,3}}}) {
        errno=0;
        if(prepare(invalid[0],invalid[1],invalid[2],invalid[3])!=-1 || errno!=EINVAL) return failed("InvalidPreparation");
    }
    if(prepare(Width,Height,Format,1)) return failed("Prepare");
    errno=0; if(prepare(Width,Height,Format,1)!=-1 || errno!=EBUSY) return failed("DuplicatePreparation");
    int peerResult=-1;
    std::thread peer([&]{ peerResult=prepare(131,75,87,1); if(!peerResult) peerResult=cancel(); }); peer.join();
    if(peerResult) return failed("ThreadPreparation");
    std::puts("EGL_NATIVE_IMAGE_PREPARATION invalidRejected=5 duplicateRejected=true perThread=true");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=Width; desc.Height=Height; desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=Format; desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask=heap.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> source;
    const auto resourceResult=native.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,
        nullptr,IID_PPV_ARGS(&source));
    if(cancel() || FAILED(resourceResult)) return failed("NativeTexture");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> rtv; ComPtr<ID3D12Fence> fence;
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors=1;
    if(FAILED(native.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
       FAILED(native.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))) ||
       FAILED(native.device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtv))) ||
       FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return failed("NativeCommands");
    auto descriptor=rtv->GetCPUDescriptorHandleForHeapStart(); native.device->CreateRenderTargetView(source.Get(),nullptr,descriptor);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=source.Get(); barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    command->ResourceBarrier(1,&barrier); const FLOAT red[4]={1,0,0,1}; command->ClearRenderTargetView(descriptor,red,0,nullptr);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;
    command->ResourceBarrier(1,&barrier);
    if(FAILED(command->Close())) return failed("NativeClose");
    ID3D12CommandList* commands[]={command.Get()}; native.queue->ExecuteCommandLists(1,commands);
    if(FAILED(native.queue->Signal(fence.Get(),1))) return failed("NativeSignal");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(fence->GetCompletedValue()<1 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    if(fence->GetCompletedValue()!=1) return failed("NativeRetirement");
    std::puts("EGL_NATIVE_IMAGE_PRODUCER dxgiClear=red transitionCommon=true gpuRetired=true implicitSync=false");
    HANDLE nativeHandle=nullptr;
    if(FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&nativeHandle))) return failed("NativeExport");
    const auto rawValue=reinterpret_cast<std::intptr_t>(nativeHandle);
    if(rawValue<0 || rawValue>INT32_MAX) return failed("NativeFd");
    const int raw=int(rawValue); constexpr std::uint64_t Bytes=std::uint64_t(Width)*Height*4;
    const auto attributes=[&](int fd) { return std::array<EGLint,13>{EGL_WIDTH,Width,EGL_HEIGHT,Height,
        EGL_LINUX_DRM_FOURCC_EXT,Fourcc,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,Width*4,EGL_NONE}; };
    const auto reject=[&](const char* reason,const EGLint* attrs,int fd) {
        eglGetError(); auto image=eglCreateImageKHR(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs);
        const auto error=eglGetError();
        if(image!=EGL_NO_IMAGE_KHR || error==EGL_SUCCESS || fcntl(fd,F_GETFD)<0) return false;
        std::printf("EGL_NATIVE_IMAGE_REJECT kind=%s error=%04x inputFdRetained=true\n",reason,error); return true;
    };
    auto attrs=attributes(raw);
    if(!reject("rawCapability",attrs.data(),raw)) return failed("RejectRaw");
    const int buffer=wrap(raw,Bytes);
    if(buffer<0 || close(raw)) return failed("BufferExport");
    int foreign=open("/dev/null",O_RDONLY|O_CLOEXEC); auto bad=attributes(foreign);
    if(foreign<0 || !reject("foreignFd",bad.data(),foreign) || close(foreign)) return failed("RejectForeign");
    attrs=attributes(buffer); bad=attrs; bad[11]+=4;
    if(!reject("stride",bad.data(),buffer)) return failed("RejectStride");
    bad=attrs; bad[9]=4; if(!reject("offset",bad.data(),buffer)) return failed("RejectOffset");
    bad=attrs; bad[1]+=1; if(!reject("dimensions",bad.data(),buffer)) return failed("RejectDimensions");
    bad=attrs; bad[5]=DRM_FORMAT_RGB565; if(!reject("format",bad.data(),buffer)) return failed("RejectFormat");
    std::vector<EGLint> linear(attrs.begin(),attrs.end()-1);
    linear.insert(linear.end(),{EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,0,EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,0,EGL_NONE});
    if(!reject("linearModifier",linear.data(),buffer)) return failed("RejectLinear");
    auto image=eglCreateImageKHR(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs.data());
    if(image==EGL_NO_IMAGE_KHR || close(buffer)) return failed("ImportImage");
    wddm_native_buffer_stats live{};
    if(stats(&live) || live.held!=1 || live.bytes!=Bytes || live.created!=baseline.created+1 || live.released!=baseline.released)
        return failed("ImportedFdReference");
    std::puts("EGL_NATIVE_IMAGE_IMPORTED nativeFdClosed=true inputDmaBufFdClosed=true retainedDmaBufReference=true");
    GLuint textures[2]={},fbos[2]={}; glGenTextures(2,textures); glGenFramebuffers(2,fbos);
    const auto bindImage=[&](unsigned n,EGLImageKHR img) {
        glBindTexture(GL_TEXTURE_2D,textures[n]); glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,img);
        glBindFramebuffer(GL_FRAMEBUFFER,fbos[n]); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[n],0);
        return glGetError()==GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    };
    if(!bindImage(0,image)) return failed("Framebuffer");
    glPixelStorei(GL_PACK_ALIGNMENT,1); std::vector<std::uint8_t> pixels(std::size_t(Width)*BandRows*4);
    const auto color=[&](const char* phase,std::array<std::uint8_t,4> rgba) {
        std::uint64_t digest=UINT64_C(14695981039346656037); unsigned bands=0;
        for(unsigned first=0;first<Height;first+=BandRows) {
            const auto rows=Height-first<BandRows ? Height-first : BandRows;
            glFinish(); glReadPixels(0,first,Width,rows,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            const auto error=glGetError();
            if(error!=GL_NO_ERROR) {
                std::printf("EGL_NATIVE_IMAGE_COLOR_ERROR phase=%s firstRow=%u glError=%04x\n",phase,first,error); return false;
            }
            for(std::size_t n=0;n<std::size_t(Width)*rows*4;++n) {
                if(pixels[n]!=rgba[n%4]) {
                    std::printf("EGL_NATIVE_IMAGE_PIXEL_MISMATCH phase=%s x=%zu y=%zu channel=%zu actual=%u expected=%u\n",
                        phase,(n/4)%Width,first+(n/4)/Width,n%4,pixels[n],rgba[n%4]); return false;
                }
                digest=(digest^pixels[n])*UINT64_C(1099511628211);
            }
            ++bands;
        }
        std::printf("EGL_NATIVE_IMAGE_PIXELS phase=%s rgba=%02x%02x%02x%02x pixels=%u bands=%u fnv1a=%016llx all=true\n",
            phase,rgba[0],rgba[1],rgba[2],rgba[3],Width*Height,bands,(unsigned long long)digest);
        return true;
    };
    if(!color("nativeProducerRead",{255,0,0,255})) return failed("ProducerPixels");
    EGLint exportFourcc=0,planes=0,stride=0,offset=0; EGLuint64KHR modifier=0; int exported=-1;
    const auto queried=eglExportDMABUFImageQueryMESA(display,image,&exportFourcc,&planes,&modifier);
    const auto exportedOk=queried ? eglExportDMABUFImageMESA(display,image,&exported,&stride,&offset) : EGL_FALSE;
    std::printf("EGL_NATIVE_IMAGE_EXPORT query=%u fourcc=%08x planes=%d modifier=%016llx export=%u validFd=%s stride=%d offset=%d\n",
        queried,unsigned(exportFourcc),planes,(unsigned long long)modifier,exportedOk,exported>=0?"true":"false",stride,offset);
    if(!queried || exportFourcc!=Fourcc || planes!=1 || modifier!=DRM_FORMAT_MOD_INVALID ||
       !exportedOk || exported<0 || stride!=Width*4 || offset)
        return failed("EglExport");
    std::uint64_t extent=0;
    if(!(fcntl(exported,F_GETFD)&FD_CLOEXEC) || info(exported,&extent) || extent!=Bytes) return failed("ExportIdentity");
    auto attrs2=attributes(exported);
    auto reimported=eglCreateImageKHR(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs2.data());
    if(reimported==EGL_NO_IMAGE_KHR || close(exported) || !bindImage(1,reimported)) return failed("EglReimport");
    if(!eglDestroyImageKHR(display,image)) return failed("DestroyOriginalImage");
    source.Reset();
    glBindFramebuffer(GL_FRAMEBUFFER,fbos[0]); glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER,fbos[1]);
    if(!color("reimportedRead",{0,255,0,255})) return failed("SharedStorage");
    std::puts("EGL_NATIVE_IMAGE_LIFETIME sourceReleased=true firstImageDestroyed=true importedTextureSiblingAlive=true");
    glBindFramebuffer(GL_FRAMEBUFFER,fbos[0]); glViewport(0,0,ShaderWidth,ShaderHeight);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES,0,3); glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER,fbos[1]);
    glReadPixels(0,0,ShaderWidth,ShaderHeight,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    if(glGetError()!=GL_NO_ERROR) return failed("ShaderReadback");
    std::printf("EGL_NATIVE_IMAGE_SHADER width=130 height=73 viewport=130x73 texture=%ux%u referenceFrame=2 topOrigin=true independentPixelAudit=pending\n",Width,Height);
    for(unsigned y=0;y<ShaderHeight;++y) {
        std::printf("EGL_NATIVE_IMAGE_PIXEL_ROW y=%u rgba=",y);
        for(unsigned x=0;x<ShaderWidth*4;++x) std::printf("%02x",pixels[std::size_t(ShaderHeight-1-y)*ShaderWidth*4+x]);
        std::putchar('\n');
    }
    if(!eglDestroyImageKHR(display,reimported)) return failed("DestroyReimportedImage");
    glBindFramebuffer(GL_FRAMEBUFFER,0); glDeleteFramebuffers(2,fbos); glBindTexture(GL_TEXTURE_2D,0); glDeleteTextures(2,textures); glFinish();
    if(glGetError()!=GL_NO_ERROR) return failed("GlCleanup");
    std::puts("EGL_NATIVE_IMAGE_COMPLETE import=true export=true reimport=true nativeProducerPixels=true sharedShaderPixels=pending desktop=false gbm=false");
    return 0;
}

extern "C" int wddm_native_image_cleanup(void) {
    const auto stats=(int(*)(wddm_native_buffer_stats*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_stats");
    wddm_native_buffer_stats value{};
    if(!stats || stats(&value) || value.held || value.bytes || value.created!=value.released ||
       value.created!=(imageEnabled ? imageBaseline.created+2 : 0)) {
        std::puts("EGL_NATIVE_IMAGE_CLEANUP failed=true beforeProcessExit=true"); return 1;
    }
    std::printf("EGL_NATIVE_IMAGE_CLEANUP enabled=%s created=%llu released=%llu held=0 bytes=0 contextsDestroyed=true beforeProcessExit=true\n",
        imageEnabled?"true":"false",(unsigned long long)value.created,(unsigned long long)value.released);
    return 0;
}
