// SPDX-License-Identifier: MIT
// Genuine GL semaphore API calls against the current private Mesa context.
// Its D3D12 device is borrowed through Mesa's existing public interop query.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>
// DirectX's Linux headers already define HDC/HANDLE. Suppress Mesa's Win32
// aliases; only the EGL interop entry point is used by this Linux diagnostic.
struct HGLRC__;
using HGLRC = HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using Microsoft::WRL::ComPtr;

static bool completed(ID3D12Fence* fence, UINT64 target) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX) return false;
        if (value >= target) return value == target;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

extern "C" int wddm_mesa_fence_fd_test(EGLDisplay display, EGLContext context) {
    std::puts("MESA_FENCE_FD_BEGIN timeline=true binary=false syncFile=false desktop=false");
#define GL_ENTRY(type, name) auto name = reinterpret_cast<type>(eglGetProcAddress(#name)); if (!name) return 1;
    GL_ENTRY(PFNGLGENSEMAPHORESEXTPROC, glGenSemaphoresEXT)
    GL_ENTRY(PFNGLCREATESEMAPHORESNVPROC, glCreateSemaphoresNV)
    GL_ENTRY(PFNGLDELETESEMAPHORESEXTPROC, glDeleteSemaphoresEXT)
    GL_ENTRY(PFNGLSEMAPHOREPARAMETERIVNVPROC, glSemaphoreParameterivNV)
    GL_ENTRY(PFNGLGETSEMAPHOREPARAMETERIVNVPROC, glGetSemaphoreParameterivNV)
    GL_ENTRY(PFNGLSEMAPHOREPARAMETERUI64VEXTPROC, glSemaphoreParameterui64vEXT)
    GL_ENTRY(PFNGLIMPORTSEMAPHOREFDEXTPROC, glImportSemaphoreFdEXT)
    GL_ENTRY(PFNGLSIGNALSEMAPHOREEXTPROC, glSignalSemaphoreEXT)
    GL_ENTRY(PFNGLWAITSEMAPHOREEXTPROC, glWaitSemaphoreEXT)
    GL_ENTRY(PFNGLGETERRORPROC, glGetError)
    GL_ENTRY(PFNGLFLUSHPROC, glFlush)
    GL_ENTRY(PFNGLGETINTEGER64VPROC, glGetInteger64v)
#undef GL_ENTRY
    const auto failure = [](const char* stage) {
        std::printf("MESA_FENCE_FD_FAILURE stage=%s\n", stage);
        return 1;
    };
    GLint64 maxDifference = 0;
    glGetInteger64v(GL_MAX_TIMELINE_SEMAPHORE_VALUE_DIFFERENCE_NV, &maxDifference);
    if (glGetError() || maxDifference <= 0) return failure("TimelineExtension");
    d3d12_interop_device_info native{};
    mesa_glinterop_device_info info{};
    info.version = 2; info.driver_data_size = sizeof native; info.driver_data = &native;
    if (MesaGLInteropEGLQueryDeviceInfo(display, context, &info) != MESA_GLINTEROP_SUCCESS ||
        info.driver_data_size != sizeof native || !native.device || !native.queue)
        return failure("MesaDeviceInterop");
    auto device = native.device;
    ComPtr<ID3D12Fence> source, control;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&source))))
        return failure("SharedFence");
    HANDLE handle = nullptr;
    if (FAILED(device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        return failure("ControlExport");
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
    const auto controlResult = device->OpenSharedHandle(handle, IID_PPV_ARGS(&control));
    if (fd >= 0) close(fd);
    if (FAILED(controlResult) || !control) return failure("ControlImport");
    if (FAILED(device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        return failure("GlExport");
    fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
    if (fd < 0) return failure("GlExportDescriptor");
    struct Semaphores {
        PFNGLDELETESEMAPHORESEXTPROC destroy;
        GLuint names[2]{};
        ~Semaphores() { destroy(2, names); }
    } semaphores{glDeleteSemaphoresEXT, {}};
    glGenSemaphoresEXT(1, &semaphores.names[0]);
    glCreateSemaphoresNV(1, &semaphores.names[1]);
    const auto binary = semaphores.names[0], timeline = semaphores.names[1];
    glImportSemaphoreFdEXT(binary, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    const auto binaryError = glGetError();
    if (binaryError != GL_INVALID_OPERATION || fcntl(fd, F_GETFD) < 0) {
        close(fd); return failure("BinaryImportOwnership");
    }
    std::puts("MESA_FENCE_FD_REJECT kind=binary error=0502 fdRetained=true");
    glSignalSemaphoreEXT(binary, 0, nullptr, 0, nullptr, nullptr);
    if (glGetError() != GL_INVALID_OPERATION) { close(fd); return failure("UnimportedSignal"); }
    glWaitSemaphoreEXT(binary, 0, nullptr, 0, nullptr, nullptr);
    if (glGetError() != GL_INVALID_OPERATION) { close(fd); return failure("UnimportedWait"); }
    std::puts("MESA_FENCE_FD_REJECT kind=unimported-wait-signal error=0502");
    const GLint timelineType = GL_SEMAPHORE_TYPE_TIMELINE_NV;
    const GLint binaryType = GL_SEMAPHORE_TYPE_BINARY_NV;
    // Pre-import type changes must not assert or alias the wrong parameter.
    glSemaphoreParameterivNV(timeline, GL_SEMAPHORE_TYPE_NV, &timelineType);
    glSemaphoreParameterivNV(timeline, GL_SEMAPHORE_TYPE_NV, &binaryType);
    glSemaphoreParameterivNV(timeline, GL_SEMAPHORE_TYPE_NV, &timelineType);
    GLint queriedType = 0;
    glGetSemaphoreParameterivNV(timeline, GL_SEMAPHORE_TYPE_NV, &queriedType);
    if (glGetError() || queriedType != timelineType) { close(fd); return failure("TimelineType"); }
    glImportSemaphoreFdEXT(timeline, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    const auto importError = glGetError();
    if (importError) { close(fd); return failure("GlImport"); }
    // Ownership transferred to GL; do not operate on the consumed descriptor.
    source.Reset();
    std::puts("MESA_FENCE_FD_IMPORT timeline=true sourceReleased=true fdOwnership=GL");
    glSemaphoreParameterivNV(timeline, GL_SEMAPHORE_TYPE_NV, &binaryType);
    if (glGetError() != GL_INVALID_OPERATION) return failure("ImportedTypeMutation");
    const int foreign = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (foreign < 0) return failure("ForeignFd");
    glImportSemaphoreFdEXT(timeline, GL_HANDLE_TYPE_OPAQUE_FD_EXT, foreign);
    const auto foreignError = glGetError();
    const bool foreignRetained = fcntl(foreign, F_GETFD) >= 0;
    close(foreign);
    if (foreignError != GL_INVALID_OPERATION || !foreignRetained) return failure("ForeignImportOwnership");
    std::puts("MESA_FENCE_FD_REJECT kind=foreign error=0502 fdRetained=true oldFenceRetained=true");
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> release;
    const auto queueResult = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&release));
    std::printf("MESA_FENCE_FD_QUEUE type=COPY status=%08x\n", static_cast<unsigned>(queueResult));
    if (FAILED(queueResult)) return failure("ReleaseQueue");
    GLuint64 value = 1;
    glSemaphoreParameterui64vEXT(timeline, GL_TIMELINE_SEMAPHORE_VALUE_NV, &value);
    glSignalSemaphoreEXT(timeline, 0, nullptr, 0, nullptr, nullptr);
    glFlush();
    if (glGetError() || !completed(control.Get(), 1)) return failure("GlSignal1");
    std::puts("MESA_FENCE_FD_SIGNAL api=GL target=1 observed=1");
    value = 2;
    glSemaphoreParameterui64vEXT(timeline, GL_TIMELINE_SEMAPHORE_VALUE_NV, &value);
    glWaitSemaphoreEXT(timeline, 0, nullptr, 0, nullptr, nullptr);
    value = 3;
    glSemaphoreParameterui64vEXT(timeline, GL_TIMELINE_SEMAPHORE_VALUE_NV, &value);
    glSignalSemaphoreEXT(timeline, 0, nullptr, 0, nullptr, nullptr);
    glFlush();
    const auto pendingError = glGetError();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto beforeRelease = control->GetCompletedValue();
    std::printf("MESA_FENCE_FD_WAIT target=2 dependentSignal=3 observedBeforeRelease=%llu\n",
        static_cast<unsigned long long>(beforeRelease));
    // Release even when the observation fails, so the diagnostic doesn't
    // deliberately leave its GPU queue stalled during failure cleanup.
    const auto releaseResult = release->Signal(control.Get(), 2);
    const bool retired = SUCCEEDED(releaseResult) && completed(control.Get(), 3);
    if (pendingError || beforeRelease != 1 || !retired) return failure("FutureQueueOrdering");
    std::puts("MESA_FENCE_FD_RELEASE api=D3D12 target=2 dependentObserved=3");
    std::puts("MESA_FENCE_FD_COMPLETE glSignals=2 gpuReleaseSignals=1 futureWaits=1 desktop=false");
    return 0;
}
