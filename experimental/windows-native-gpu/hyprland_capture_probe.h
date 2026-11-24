// SPDX-License-Identifier: MIT
// Optional bounded capture mode for the actual-compositor startup probe.
#pragma once
static int capture_output(int bufferFd,uint32_t width,uint32_t height,uint32_t format,uint32_t stride) {
    void* (*currentDisplay)(void)=(void* (*)(void))dlsym(RTLD_DEFAULT,"eglGetCurrentDisplay");
    void* (*proc)(const char*)=(void* (*)(const char*))dlsym(RTLD_DEFAULT,"eglGetProcAddress");
    void (*flush)(void)=(void (*)(void))dlsym(RTLD_DEFAULT,"glFlush");
    int (*capture)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*)=
        (int (*)(int,int,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_capture_compositor_buffer");
    if(!currentDisplay || !proc || !flush || !capture) { puts("HYPRLAND_HOST_CAPTURE_FAILURE reason=missing-entrypoint"); return -1; }
    void* display=currentDisplay();
    void* (*create)(void*,unsigned,const int*)=(void* (*)(void*,unsigned,const int*))proc("eglCreateSyncKHR");
    unsigned (*destroy)(void*,void*)=(unsigned (*)(void*,void*))proc("eglDestroySyncKHR");
    int (*duplicate)(void*,void*)=(int (*)(void*,void*))proc("eglDupNativeFenceFDANDROID");
    if(!display || !create || !destroy || !duplicate) { puts("HYPRLAND_HOST_CAPTURE_FAILURE reason=missing-native-fence"); return -1; }
    void* fence=create(display,0x3144,NULL); /* EGL_SYNC_NATIVE_FENCE_ANDROID */
    if(!fence) { puts("HYPRLAND_HOST_CAPTURE_FAILURE reason=create-native-fence"); return -1; }
    flush();
    const int fd=duplicate(display,fence);
    if(fd<0) { destroy(display,fence); puts("HYPRLAND_HOST_CAPTURE_FAILURE reason=duplicate-native-fence"); return -1; }
    uint64_t hash=0;
    puts("HYPRLAND_HOST_CAPTURE_BEGIN outputBuffer=true nativeFence=true guestCpuMap=false windowsPresentation=false");
    const int result=capture(bufferFd,fd,width,height,format,stride,&hash);
    const int error=errno;
    const int closed=close(fd);
    const unsigned destroyed=destroy(display,fence);
    printf("HYPRLAND_HOST_CAPTURE_RESULT result=%d errno=%d fnv1a=%016llx syncFileClosed=%s eglSyncDestroyed=%s windowsPresentation=false\n",
        result,result?error:0,(unsigned long long)hash,closed?"false":"true",destroyed?"true":"false");
    return result || closed || !destroyed ? -1 : 0;
}
