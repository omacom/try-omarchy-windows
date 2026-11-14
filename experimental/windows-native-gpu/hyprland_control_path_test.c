// SPDX-License-Identifier: MIT
// Test the actual path helper without installing the compositor interposer.
#define __libc_start_main unused_compositor_interposer_entry
#include "hyprland_startup_probe.c"
#undef __libc_start_main

int main(void) {
    char path[108];
    if(setenv("HYPRLAND_INSTANCE_SIGNATURE","test-instance",1)) return 1;
    const char* roots[]={"/tmp/hypr-runtime","/run/user/1000"};
    for(unsigned index=0;index<2;++index) {
        char expected[108];
        if(setenv("XDG_RUNTIME_DIR",roots[index],1) || control_socket_path(path,sizeof path)) return 1;
        snprintf(expected,sizeof expected,"%s/hypr/test-instance/.socket.sock",roots[index]);
        if(strcmp(path,expected)) return 1;
    }
    if(!control_socket_path(path,8)) return 1;
    if(setenv("XDG_RUNTIME_DIR","relative",1) || !control_socket_path(path,sizeof path)) return 1;
    if(unsetenv("XDG_RUNTIME_DIR") || !control_socket_path(path,sizeof path)) return 1;
    if(setenv("XDG_RUNTIME_DIR","/run/user/1000",1) || setenv("HYPRLAND_INSTANCE_SIGNATURE","",1) ||
       !control_socket_path(path,sizeof path)) return 1;
    puts("PASS: actual compositor control path follows XDG_RUNTIME_DIR; missing/relative/truncated inputs rejected; gpu=false");
    return 0;
}
