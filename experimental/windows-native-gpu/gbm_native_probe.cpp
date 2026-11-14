// SPDX-License-Identifier: MIT
// Exercise the real DRM/GBM allocation route, not KMS or desktop acceptance.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <gbm.h>
#include <xf86drm.h>
#include "guest-kernel/wddm_aperture.h"
#include "guest-kernel/wddm_render.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include <execinfo.h>
#include <signal.h>

static int renderFd=-1;
static gbm_device* device=nullptr;
static GLuint textures[2]{}, fbos[2]{};
static wddm_native_buffer_stats baseline{};
static PFNGLBINDTEXTUREPROC bindTexture;
static PFNGLBINDFRAMEBUFFERPROC bindFramebuffer;
static PFNGLDELETEFRAMEBUFFERSPROC deleteFramebuffers;
static PFNGLDELETETEXTURESPROC deleteTextures;
static PFNGLFINISHPROC finish;
static PFNGLGETERRORPROC getError;

static int bad(const char* stage) {
    std::printf("GBM_NATIVE_FAILURE stage=%s eglError=%04x errno=%d\n",stage,eglGetError(),errno);
    return 1;
}
static void traceCrash(int signalNumber) {
    void* frames[32]; const int count=backtrace(frames,32);
    const char marker[]="GBM_NATIVE_FAILURE fatalSignal=11 stackFollows=true\n";
    const auto written=write(STDERR_FILENO,marker,sizeof(marker)-1);
    if(written>0) backtrace_symbols_fd(frames,count,STDERR_FILENO);
    signal(signalNumber,SIG_DFL); kill(getpid(),signalNumber);
}
static bool contract(int fd) {
    for(unsigned n=0;n<7;++n) {
        wddm_render_info info{}; info.version=1;
        if(n==0) info.version=0;
        if(n==1) info.flags=1;
        if(n==2) info.signature=1;
        if(n==3) info.max_width=1;
        if(n==4) info.max_height=1;
        if(n==5) info.reserved[0]=1;
        if(n==6) info.reserved[1]=1;
        const auto before=info; errno=0;
        if(ioctl(fd,DRM_IOCTL_WDDM_RENDER_INFO,&info)!=-1 || errno!=EINVAL || std::memcmp(&info,&before,sizeof info)) return false;
    }
    void* page=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(page==MAP_FAILED) return false;
    auto* readOnly=static_cast<wddm_render_info*>(page); *readOnly={}; readOnly->version=1;
    if(mprotect(page,4096,PROT_READ)) { munmap(page,4096); return false; }
    errno=0; const auto copied=ioctl(fd,DRM_IOCTL_WDDM_RENDER_INFO,readOnly); const int error=errno;
    munmap(page,4096);
    if(copied!=-1 || error!=EFAULT) return false;
    for(std::uint64_t capability:{DRM_CAP_DUMB_BUFFER,DRM_CAP_PRIME,DRM_CAP_SYNCOBJ,DRM_CAP_SYNCOBJ_TIMELINE}) {
        std::uint64_t value=UINT64_MAX;
        if(drmGetCap(fd,capability,&value) || value) return false;
    }
    drm_gem_close closeObject{}; closeObject.handle=1; errno=0;
    if(ioctl(fd,DRM_IOCTL_GEM_CLOSE,&closeObject)!=-1 || errno!=ENOTTY) return false;
    errno=0; void* mapped=mmap(nullptr,4096,PROT_READ,MAP_SHARED,fd,0);
    if(mapped!=MAP_FAILED) { munmap(mapped,4096); return false; }
    if(errno!=ENODEV) return false;
    std::puts("GBM_NATIVE_CONTRACT malformedInfo=7 readOnlyCopyout=true kernelPrime=false kernelSyncobj=false gemRejected=true cpuMapRejected=true");
    return true;
}

extern "C" EGLDisplay wddm_gbm_native_display(void) {
    void* warmup[1]; (void)backtrace(warmup,1);
    signal(SIGSEGV,traceCrash);
    const char* path=std::getenv("WDDM_BRIDGE_RENDER_NODE");
    if(!path || std::strncmp(path,"/dev/dri/renderD",16)) { bad("NodePath"); return EGL_NO_DISPLAY; }
    renderFd=open(path,O_RDWR|O_CLOEXEC);
    if(renderFd<0) { bad("OpenNode"); return EGL_NO_DISPLAY; }
    wddm_render_info info{}; info.version=1;
    if(ioctl(renderFd,DRM_IOCTL_WDDM_RENDER_INFO,&info) || info.flags!=1 || info.signature!=WDDM_RENDER_MAGIC ||
        info.max_width!=4096 || info.max_height!=4096 || info.reserved[0] || info.reserved[1] || !contract(renderFd)) {
        bad("NodeIdentity"); return EGL_NO_DISPLAY;
    }
    auto* version=drmGetVersion(renderFd);
    if(!version || !version->name || std::strcmp(version->name,"d3d12")) {
        if(version) drmFreeVersion(version);
        bad("DrmVersion"); return EGL_NO_DISPLAY;
    }
    drmFreeVersion(version);
    setenv("GBM_BACKENDS_PATH","/opt/wddm-mesa/lib/gbm",1);
    setenv("GBM_BACKEND","dri",1);
    device=gbm_create_device(renderFd);
    if(!device) { bad("CreateDevice"); return EGL_NO_DISPLAY; }
    char* name=drmGetDeviceNameFromFd2(renderFd);
    if(!name) { bad("DrmNodeName"); return EGL_NO_DISPLAY; }
    std::printf("GBM_NATIVE_DEVICE node=%s drmName=%s backend=%s realDrmFd=true desktop=false\n",path,name,gbm_device_get_backend_name(device));
    free(name);
    return eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,device,nullptr);
}

extern "C" int wddm_gbm_native_bind(EGLDisplay display) {
#define PROC(type,name) const auto name=(type)eglGetProcAddress(#name); if(!name) return bad(#name)
    PROC(PFNEGLCREATEIMAGEKHRPROC,eglCreateImageKHR);
    PROC(PFNEGLDESTROYIMAGEKHRPROC,eglDestroyImageKHR);
    PROC(PFNGLGENTEXTURESPROC,glGenTextures);
    PROC(PFNGLBINDTEXTUREPROC,glBindTexture);
    PROC(PFNGLTEXPARAMETERIPROC,glTexParameteri);
    using ImageTarget=void(*)(GLenum,void*);
    PROC(ImageTarget,glEGLImageTargetTexture2DOES);
    PROC(PFNGLGENFRAMEBUFFERSPROC,glGenFramebuffers);
    PROC(PFNGLBINDFRAMEBUFFERPROC,glBindFramebuffer);
    PROC(PFNGLFRAMEBUFFERTEXTURE2DPROC,glFramebufferTexture2D);
    PROC(PFNGLCHECKFRAMEBUFFERSTATUSPROC,glCheckFramebufferStatus);
    PROC(PFNGLDELETEFRAMEBUFFERSPROC,glDeleteFramebuffers);
    PROC(PFNGLDELETETEXTURESPROC,glDeleteTextures);
    PROC(PFNGLCLEARCOLORPROC,glClearColor);
    PROC(PFNGLCLEARPROC,glClear);
    PROC(PFNGLREADPIXELSPROC,glReadPixels);
    PROC(PFNGLFINISHPROC,glFinish);
    PROC(PFNGLGETERRORPROC,glGetError);
#undef PROC
    bindTexture=glBindTexture; bindFramebuffer=glBindFramebuffer;
    deleteFramebuffers=glDeleteFramebuffers; deleteTextures=glDeleteTextures;
    finish=glFinish; getError=glGetError;
    const auto stats=(int(*)(wddm_native_buffer_stats*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_stats");
    const auto info=(int(*)(int,std::uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info");
    if(!stats || !info || stats(&baseline) || baseline.held || baseline.created!=baseline.released) return bad("Baseline");
    struct Description {unsigned width,height,format;};
    const Description descriptions[2]={{1281,719,GBM_FORMAT_ARGB8888},{257,131,GBM_FORMAT_ABGR8888}};
    gbm_bo* bos[2]{};
    for(unsigned n=0;n<2;++n) {
        const auto& d=descriptions[n];
        if(!gbm_device_is_format_supported(device,d.format,GBM_BO_USE_RENDERING)) return bad("FormatSupport");
        bos[n]=gbm_bo_create(device,d.width,d.height,d.format,GBM_BO_USE_RENDERING);
        if(!bos[n]) return bad("CreateBo");
    }
    glGenTextures(2,textures); glGenFramebuffers(2,fbos);
    for(unsigned n=0;n<2;++n) {
        const auto& d=descriptions[n];
        if(gbm_bo_get_plane_count(bos[n])!=1 || gbm_bo_get_stride_for_plane(bos[n],0)!=d.width*4 ||
            gbm_bo_get_offset(bos[n],0) || gbm_bo_get_modifier(bos[n])!=UINT64_C(0x00ffffffffffffff)) return bad("BoMetadata");
        const int fd=gbm_bo_get_fd_for_plane(bos[n],0);
        std::uint64_t extent=0;
        if(fd<0 || !(fcntl(fd,F_GETFD)&FD_CLOEXEC) || info(fd,&extent) || extent!=std::uint64_t(d.width)*d.height*4) return bad("BoFd");
        EGLint attrs[]={EGL_WIDTH,int(d.width),EGL_HEIGHT,int(d.height),EGL_LINUX_DRM_FOURCC_EXT,int(d.format),
            EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,int(d.width*4),EGL_NONE};
        auto image=eglCreateImageKHR(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs);
        if(image==EGL_NO_IMAGE_KHR || close(fd)) return bad("ImportBo");
        glBindTexture(GL_TEXTURE_2D,textures[n]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,image);
        glBindFramebuffer(GL_FRAMEBUFFER,fbos[n]);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[n],0);
        if(glGetError()!=GL_NO_ERROR || glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
            !eglDestroyImageKHR(display,image)) return bad("Framebuffer");
        gbm_bo_destroy(bos[n]); bos[n]=nullptr;
        const std::array<std::uint8_t,4> expected=n ? std::array<std::uint8_t,4>{0,255,0,255} : std::array<std::uint8_t,4>{255,0,0,255};
        glClearColor(expected[0]/255.f,expected[1]/255.f,expected[2]/255.f,1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
        std::vector<std::uint8_t> pixels(std::size_t(d.width)*d.height*4);
        glReadPixels(0,0,d.width,d.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        if(glGetError()!=GL_NO_ERROR) return bad("Readback");
        std::uint64_t hash=UINT64_C(14695981039346656037);
        for(std::size_t i=0;i<pixels.size();++i) {
            if(pixels[i]!=expected[i%4]) return bad("StoragePixels");
            hash=(hash^pixels[i])*UINT64_C(1099511628211);
        }
        std::printf("GBM_NATIVE_BUFFER index=%u width=%u height=%u fourcc=%08x stride=%u modifier=00ffffffffffffff nativeDmaBuf=true sourceBoDestroyed=true imageDestroyed=true textureAlive=true verifiedPixels=%u rgba=%02x%02x%02x%02x fnv1a=%016llx\n",
            n,d.width,d.height,d.format,d.width*4,d.width*d.height,expected[0],expected[1],expected[2],expected[3],(unsigned long long)hash);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,fbos[0]);
    std::puts("GBM_NATIVE_BOUND shaderTarget=0 viewport=130x73 texture=1281x719 concurrentBos=2 kms=false desktop=false");
    return 0;
}

extern "C" int wddm_gbm_native_sample(void) {
#define PROC(type,name) const auto name=(type)eglGetProcAddress(#name); if(!name) return bad(#name)
    PROC(PFNGLCREATESHADERPROC,glCreateShader);
    PROC(PFNGLSHADERSOURCEPROC,glShaderSource);
    PROC(PFNGLCOMPILESHADERPROC,glCompileShader);
    PROC(PFNGLGETSHADERIVPROC,glGetShaderiv);
    PROC(PFNGLDELETESHADERPROC,glDeleteShader);
    PROC(PFNGLCREATEPROGRAMPROC,glCreateProgram);
    PROC(PFNGLATTACHSHADERPROC,glAttachShader);
    PROC(PFNGLLINKPROGRAMPROC,glLinkProgram);
    PROC(PFNGLGETPROGRAMIVPROC,glGetProgramiv);
    PROC(PFNGLUSEPROGRAMPROC,glUseProgram);
    PROC(PFNGLGETUNIFORMLOCATIONPROC,glGetUniformLocation);
    PROC(PFNGLUNIFORM1IPROC,glUniform1i);
    PROC(PFNGLDELETEPROGRAMPROC,glDeleteProgram);
    PROC(PFNGLGETINTEGERVPROC,glGetIntegerv);
    PROC(PFNGLACTIVETEXTUREPROC,glActiveTexture);
    PROC(PFNGLDRAWARRAYSPROC,glDrawArrays);
    PROC(PFNGLREADPIXELSPROC,glReadPixels);
#undef PROC
    const char* vertexSource="#version 330 core\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}\n";
    const char* fragmentSource="#version 330 core\nuniform sampler2D sourceImage;layout(location=0) out vec4 color;void main(){color=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),0);}\n";
    const auto compile=[&](GLenum type,const char* source) {
        const GLuint shader=glCreateShader(type); glShaderSource(shader,1,&source,nullptr); glCompileShader(shader);
        GLint ready=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&ready);
        if(!ready) { glDeleteShader(shader); return GLuint{0}; }
        return shader;
    };
    const auto vertex=compile(GL_VERTEX_SHADER,vertexSource), fragment=compile(GL_FRAGMENT_SHADER,fragmentSource);
    if(!vertex || !fragment) return bad("SampleShaders");
    const GLuint program=glCreateProgram(); glAttachShader(program,vertex); glAttachShader(program,fragment); glLinkProgram(program);
    GLint linked=0,previous=0; glGetProgramiv(program,GL_LINK_STATUS,&linked); glGetIntegerv(GL_CURRENT_PROGRAM,&previous);
    if(!linked) return bad("SampleLink");
    glUseProgram(program); const auto source=glGetUniformLocation(program,"sourceImage");
    if(source<0) return bad("SampleUniform");
    glUniform1i(source,0); glActiveTexture(GL_TEXTURE0); bindTexture(GL_TEXTURE_2D,textures[0]);
    bindFramebuffer(GL_FRAMEBUFFER,fbos[1]); glDrawArrays(GL_TRIANGLES,0,3); finish();
    std::array<std::uint8_t,130*73*4> pixels{};
    glReadPixels(0,0,130,73,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    if(getError()!=GL_NO_ERROR) return bad("SampleDraw");
    std::puts("GBM_NATIVE_SAMPLED source=0 destination=1 sourceFourcc=34325241 destinationFourcc=34324241 width=130 height=73 referenceFrame=2 topOrigin=true independentPixelAudit=pending");
    for(unsigned y=0;y<73;++y) {
        std::printf("GBM_NATIVE_SAMPLED_PIXEL_ROW y=%u rgba=",y);
        for(unsigned x=0;x<130*4;++x) std::printf("%02x",pixels[(72-y)*130*4+x]);
        std::putchar('\n');
    }
    glUseProgram(previous); glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    std::puts("GBM_NATIVE_SAMPLING_COMPLETE importedTextureSampled=true distinctDestination=true softwarePixels=false independentPixelAudit=pending");
    return getError()==GL_NO_ERROR ? 0 : bad("SampleCleanup");
}

extern "C" int wddm_gbm_native_cleanup(void) {
    bindFramebuffer(GL_FRAMEBUFFER,0); deleteFramebuffers(2,fbos);
    bindTexture(GL_TEXTURE_2D,0); deleteTextures(2,textures); finish();
    return getError()==GL_NO_ERROR ? 0 : bad("GlCleanup");
}
extern "C" int wddm_gbm_native_close(void) {
    gbm_device_destroy(device); device=nullptr;
    if(fcntl(renderFd,F_GETFD)<0 || close(renderFd)) return bad("CallerFdOwnership");
    renderFd=-1;
    const auto stats=(int(*)(wddm_native_buffer_stats*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_stats");
    wddm_native_buffer_stats value{};
    if(!stats || stats(&value) || value.held || value.bytes || value.created!=baseline.created+2 || value.created!=value.released) return bad("BufferCleanup");
    std::printf("GBM_NATIVE_COMPLETE allocation=true fdExport=true eglImport=true shaderAudit=pending created=%llu released=%llu held=0 bytes=0 beforeProcessExit=true desktop=false\n",
        (unsigned long long)value.created,(unsigned long long)value.released);
    return 0;
}
