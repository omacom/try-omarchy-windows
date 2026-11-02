// SPDX-License-Identifier: MIT
// Guest PID1-only payload installation; included only by the explicit normal
// session bootstrap build. The default diagnostic bootstrap is unchanged.
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include "normal_session_command_line.h"

static void normal_readonly_bind(const char* source,const char* target) {
    if(mount(source,target,NULL,MS_BIND,NULL) ||
       mount(NULL,target,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL))
        fail("normal-session-readonly-bind");
}
static uint64_t normal_boot_time(void) {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) fail("normal-session-clock");
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}
static void normal_copy_overlay(void) {
    const pid_t child=fork();
    if(child<0) fail("normal-session-overlay-fork");
    if(!child) {
        if(chroot("/newroot") || chdir("/")) _exit(125);
        // The immutable official overlay supplies real Unix modes and symlinks.
        // Use the guest's normal coreutils, exactly as its rootfs builder does.
        char* const arguments[]={"/usr/bin/cp","-a","--no-preserve=ownership",
            "/run/omarchy-nvgpu-bootstrap/.","/",NULL};
        char* const environment[]={"PATH=/usr/bin:/bin","LANG=C",NULL};
        execve(arguments[0],arguments,environment);_exit(125);
    }
    const uint64_t deadline=normal_boot_time()+30000000000ull;
    int status=0;
    for(;;) {
        const pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child) break;
        if(result<0 && errno!=EINTR) fail("normal-session-overlay-wait");
        if(normal_boot_time()>=deadline) {
            // This is our own unreaped child, so its PID cannot be recycled.
            const int stopped=kill(child,SIGKILL);(void)stopped;
            errno=ETIMEDOUT;fail("normal-session-overlay-timeout");
        }
        const struct timespec delay={.tv_sec=0,.tv_nsec=10000000};
        if(nanosleep(&delay,NULL) && errno!=EINTR) fail("normal-session-overlay-delay");
    }
    if(!WIFEXITED(status) || WEXITSTATUS(status)) { errno=EIO;fail("normal-session-overlay-copy"); }
}
static void graphics_payload(void) {
    const int command=open("/proc/cmdline",O_RDONLY|O_CLOEXEC);
    char text[8192];
    const ssize_t length=command>=0 ? read(command,text,sizeof(text)-1) : -1;
    if(length<=0 || length>=(ssize_t)sizeof(text)-1 || close(command)) fail("normal-session-command-line");
    text[length]=0;
    if(!wddm_normal_session_selected(text)) { errno=EINVAL;fail("normal-session-opt-in"); }
    directory("/newroot/opt",0755);
    if(mkdir("/newroot/opt/omarchy-nvgpu",0755)) fail("normal-session-runtime-directory");
    normal_readonly_bind("/normal-root-overlay","/newroot/opt/omarchy-nvgpu");
    directory("/newroot/run",0755);
    if(mkdir("/newroot/run/omarchy-nvgpu-bootstrap",0700)) fail("normal-session-overlay-directory");
    normal_readonly_bind("/normal-session-overlay","/newroot/run/omarchy-nvgpu-bootstrap");
    normal_copy_overlay();
    if(umount2("/newroot/run/omarchy-nvgpu-bootstrap",0) || rmdir("/newroot/run/omarchy-nvgpu-bootstrap"))
        fail("normal-session-overlay-unmount");
    directory("/newroot/usr/lib/wsl",0755);
    const char* links[][2]={{"/usr/lib/x86_64-linux-gnu","/opt/omarchy-nvgpu/lib"},
        {"/usr/lib/wsl/lib","/opt/omarchy-nvgpu/wsl-libs"},
        {"/usr/lib/wsl/drivers","/opt/omarchy-nvgpu/wsl-drivers"},
        {"/opt/wddm-mesa","/opt/omarchy-nvgpu/mesa"}};
    for(unsigned index=0;index<sizeof(links)/sizeof(links[0]);++index) {
        char path[256];
        if(snprintf(path,sizeof path,"/newroot%s",links[index][0])>=(int)sizeof path || symlink(links[index][1],path))
            fail("normal-session-runtime-alias");
    }
    puts("NORMAL_SESSION_PAYLOAD_STAGED runtimeReadonly=true officialProvisioningRetained=true accountCreatedByBootstrap=false finiteWorkload=false");
}
