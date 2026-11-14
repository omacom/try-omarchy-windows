// SPDX-License-Identifier: MIT
// Guest root service: original SDDM/UWSM login owns the compositor lifetime.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "guest_session_admission.h"
#include "guest_session_owner.h"
#ifndef WDDM_SESSION_PREPARE_DEVICES
#include "guest_session_devices.h"
#define WDDM_SESSION_PREPARE_DEVICES wddm_session_devices_ready
#endif
#ifndef WDDM_SESSION_ROOT
#define WDDM_SESSION_ROOT "/run/omarchy-nvgpu"
#endif
#ifndef WDDM_SESSION_OWNER_FILE
#define WDDM_SESSION_OWNER_FILE "/etc/sddm.conf.d/autologin.conf"
#endif
#ifndef WDDM_SESSION_WRAPPER
#define WDDM_SESSION_WRAPPER "/opt/omarchy-nvgpu/bin/hyprland-session-exec"
#endif
#ifndef WDDM_SESSION_BROKER
#define WDDM_SESSION_BROKER "/opt/omarchy-nvgpu/bin/guest-graphics-broker"
#endif
#ifndef WDDM_SESSION_BROKER_DIRECTORY
#define WDDM_SESSION_BROKER_DIRECTORY "/run/wddm-broker"
#endif
#ifndef WDDM_SESSION_PRELOAD
#define WDDM_SESSION_PRELOAD "/opt/omarchy-nvgpu/lib/linux-ioctl-bridge.so:/opt/omarchy-nvgpu/lib/hyprland-session.so"
#endif
#ifndef WDDM_SESSION_UMD_FILE
#define WDDM_SESSION_UMD_FILE "/opt/omarchy-nvgpu/linux-umd-name"
#endif
struct wddm_root_runtime {
    struct wddm_session_owner owner;
    int listener,stop,image,broker_image,primary,broker_process;
    pid_t broker;
    struct stat environment_identity,socket_identity;
    int environment_created,socket_created,directory_created,broker_directory_created;
    char cgroup[256],port[128],render[64];
};
static int trusted_image(const char* path) {
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return -1;
    struct stat state;unsigned char signature[4];
    if(fstat(fd,&state) || !S_ISREG(state.st_mode) || state.st_uid || (state.st_mode&0022) ||
       !(state.st_mode&0111) || state.st_nlink!=1 || pread(fd,signature,sizeof signature,0)!=4 || memcmp(signature,"\177ELF",4)) {
        close(fd);errno=EPERM;return -1;
    }
    return fd;
}
static int root_notify(void) {
    const char* path=getenv("NOTIFY_SOCKET");if(!path) return 0;
    struct sockaddr_un address={.sun_family=AF_UNIX};const size_t length=strlen(path);
    if(length<2 || length>=sizeof address.sun_path || (path[0]!='/' && path[0]!='@')) return -1;
    memcpy(address.sun_path,path,length+1);if(path[0]=='@') address.sun_path[0]=0;
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0) return -1;
    static const char ready[]="READY=1\nSTATUS=Waiting for the configured Omarchy compositor";
    const int result=sendto(fd,ready,sizeof ready-1,MSG_DONTWAIT|MSG_NOSIGNAL,(struct sockaddr*)&address,
        (socklen_t)(offsetof(struct sockaddr_un,sun_path)+length+(path[0]=='@'?0:1)))==(ssize_t)sizeof ready-1?0:-1;
    const int closed=close(fd);return result || closed?-1:0;
}
static int write_environment(struct wddm_root_runtime* runtime) {
    char module[256];
    if(wddm_session_read_root_file(WDDM_SESSION_UMD_FILE,module,sizeof module)) return -1;
    module[strcspn(module,"\r\n")]=0;
    if(!*module || strlen(module)>128 || strspn(module,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=strlen(module)) return -1;
    const char* size=getenv("WDDM_BRIDGE_SESSION_SIZE");if(!size) size="1920x1080";
    if(strcmp(size,"1920x1080") && strcmp(size,"3840x2160")) return -1;
    char data[4096];const int length=snprintf(data,sizeof data,
        "WDDM_SESSION_VERSION=1\nWDDM_SESSION_UID=%u\nWDDM_SESSION_GID=%u\n"
        "LD_PRELOAD=\"%s\"\nLD_LIBRARY_PATH=\"/opt/wddm-mesa/lib:/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu\"\n"
        "D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE=\"true\"\nD3D12_EXPERIMENTAL_NATIVE_BUFFERS=\"true\"\n"
        "WDDM_BRIDGE_COMPOSITOR_SESSION=\"1\"\n"
        "GALLIUM_DRIVER=\"d3d12\"\nLIBGL_DRIVERS_PATH=\"/opt/wddm-mesa/lib/dri\"\n"
        "GBM_BACKENDS_PATH=\"/opt/wddm-mesa/lib/gbm\"\n"
        "WDDM_BRIDGE_KERNEL_APERTURE=\"1\"\nWDDM_BRIDGE_RENDER_NODE=\"%s\"\n"
        "WDDM_BRIDGE_BROKER_SOCKET=\"%s/graphics.sock\"\nWDDM_BRIDGE_LINUX_UMD_NAME=\"%s\"\n"
        "AQ_EXPERIMENTAL_NATIVE_RENDER=\"1\"\nWDDM_BRIDGE_NATIVE_INPUT=\"1\"\n"
        "WDDM_BRIDGE_ACTOR_PROFILE=\"14\"\nMESA_D3D12_DEFAULT_ADAPTER_NAME=\"NVIDIA\"\nWDDM_BRIDGE_SESSION_SIZE=\"%s\"\n",
        (unsigned)runtime->owner.uid,(unsigned)runtime->owner.gid,WDDM_SESSION_PRELOAD,runtime->render,WDDM_SESSION_BROKER_DIRECTORY,module,size);
    if(length<=0 || (size_t)length>=sizeof data) return -1;
    const int fd=open(WDDM_SESSION_ROOT "/compositor.env",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    runtime->environment_created=1;
    const int result=fstat(fd,&runtime->environment_identity) || write(fd,data,(size_t)length)!=length || fchmod(fd,0644);
    const int closed=close(fd);return result || closed?-1:0;
}
static int broker_reap(struct wddm_root_runtime* runtime,int force) {
    if(runtime->primary>=0) { close(runtime->primary);runtime->primary=-1; }
    if(runtime->broker<=0) return 0;
    if(force && runtime->broker_process>=0) syscall(SYS_pidfd_send_signal,runtime->broker_process,SIGTERM,NULL,0);
    const uint64_t deadline=wddm_activation_now()+(force?2000000000ull:12000000000ull);
    int status=0,reaped=0;
    while(wddm_activation_now()<deadline) {
        const pid_t result=waitpid(runtime->broker,&status,WNOHANG);
        if(result==runtime->broker) { reaped=1;break; }
        if(result<0 && errno!=EINTR) break;
        struct pollfd item={runtime->broker_process,POLLIN,0};poll(&item,1,20);
    }
    if(!reaped) {
        if(runtime->broker_process>=0) syscall(SYS_pidfd_send_signal,runtime->broker_process,SIGKILL,NULL,0);
        const uint64_t killed=wddm_activation_now()+2000000000ull;
        while(wddm_activation_now()<killed) {
            const pid_t result=waitpid(runtime->broker,&status,WNOHANG);
            if(result==runtime->broker) { reaped=1;break; }
            if(result<0 && errno!=EINTR) break;
            struct pollfd item={runtime->broker_process,POLLIN,0};poll(&item,1,20);
        }
    }
    if(!reaped) return -1; // Keep ownership; systemd's failed unit must contain it.
    runtime->broker=-1;
    if(runtime->broker_process>=0) { close(runtime->broker_process);runtime->broker_process=-1; }
    return reaped && WIFEXITED(status) && WEXITSTATUS(status)==0?0:-1;
}
static int broker_start(struct wddm_root_runtime* runtime,struct wddm_session_peer* peer,uint64_t deadline) {
    int notification[2],identity[2];
    if(pipe2(notification,O_CLOEXEC|O_NONBLOCK)) return -1;
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,identity)) { close(notification[0]);close(notification[1]);return -1; }
    if(mkdir(WDDM_SESSION_BROKER_DIRECTORY,0755)) { close(notification[0]);close(notification[1]);close(identity[0]);close(identity[1]);return -1; }
    runtime->broker_directory_created=1;
    if(chmod(WDDM_SESSION_BROKER_DIRECTORY,0755)) { close(notification[0]);close(notification[1]);close(identity[0]);close(identity[1]);return -1; }
    const pid_t child=fork();
    if(!child) {
        sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,NULL);
        close(notification[0]);close(identity[1]);
        if(fcntl(notification[1],F_SETFD,0) || fcntl(identity[0],F_SETFD,0) || clearenv()) _exit(127);
        if(setenv("LD_LIBRARY_PATH","/usr/lib/x86_64-linux-gnu",1) ||
           setenv("WDDM_BRIDGE_COORDINATED_REFERENCES","1",1) || setenv("WDDM_BRIDGE_COORDINATED_RETIREMENT","1",1) ||
           setenv("WDDM_BRIDGE_BROKER_SESSION","1",1) || setenv("WDDM_BRIDGE_RETIREMENT_WITNESSES","0",1)) _exit(127);
        char uid[16],gid[16],ready[16],primary[16];
        snprintf(uid,sizeof uid,"%u",(unsigned)runtime->owner.uid);snprintf(gid,sizeof gid,"%u",(unsigned)runtime->owner.gid);
        snprintf(ready,sizeof ready,"%d",notification[1]);snprintf(primary,sizeof primary,"%d",identity[0]);
        char* args[]={"guest-graphics-broker",WDDM_SESSION_BROKER_DIRECTORY "/graphics.sock",runtime->port,uid,gid,ready,"--coordinator",primary,NULL};
        extern char** environ;fexecve(runtime->broker_image,args,environ);_exit(127);
    }
    close(notification[1]);close(identity[0]);runtime->primary=identity[1];runtime->broker=child;
    if(child<=0) { close(notification[0]);return -1; }
    runtime->broker_process=(int)syscall(SYS_pidfd_open,child,0);
    if(runtime->broker_process<0) {
        // This is our unreaped fork child, whose PID cannot yet be recycled.
        kill(child,SIGKILL);
        int status=0;pid_t joined;
        do { joined=waitpid(child,&status,0); } while(joined<0 && errno==EINTR);
        if(joined==child) runtime->broker=-1;
        close(notification[0]);close(runtime->primary);runtime->primary=-1;return -1;
    }
    int result=-1;
    if(runtime->broker_process>=0 && !fcntl(runtime->broker_process,F_SETFD,FD_CLOEXEC) &&
       !wddm_session_wait(notification[0],peer->process,runtime->stop,POLLIN,deadline)) {
        char marker=0;
        if(read(notification[0],&marker,1)==1 && marker=='R' && wddm_primary_live(peer->process) &&
           !wddm_primary_send(runtime->primary,peer->pid,peer->process)) result=0;
    }
    close(notification[0]);close(runtime->primary);runtime->primary=-1;
    return result;
}
static int unlink_owned(const char* path,const struct stat* expected) {
    struct stat actual;
    if(lstat(path,&actual)) return errno==ENOENT?0:-1;
    if(actual.st_dev!=expected->st_dev || actual.st_ino!=expected->st_ino) return -1;
    return unlink(path);
}
static int stop_peer(struct wddm_session_peer* peer) {
    if(!wddm_primary_live(peer->process)) return 0;
    if(syscall(SYS_pidfd_send_signal,peer->process,SIGTERM,NULL,0) && errno!=ESRCH) return -1;
    struct pollfd exited={peer->process,POLLIN,0};
    if(poll(&exited,1,2000)==1) return 0;
    if(syscall(SYS_pidfd_send_signal,peer->process,SIGKILL,NULL,0) && errno!=ESRCH) return -1;
    exited.revents=0;return poll(&exited,1,2000)==1?0:-1;
}
int main(void) {
    if(getpid()==1 || getuid() || geteuid() || getgid() || getegid()) return 125;
    struct wddm_root_runtime runtime={.listener=-1,.stop=-1,.image=-1,.broker_image=-1,.primary=-1,.broker_process=-1,.broker=-1};
    struct wddm_session_peer peer={.process=-1};int result=125;
    sigset_t signals;sigemptyset(&signals);sigaddset(&signals,SIGTERM);sigaddset(&signals,SIGINT);
    if(wddm_session_owner_load(WDDM_SESSION_OWNER_FILE,&runtime.owner) || WDDM_SESSION_PREPARE_DEVICES(runtime.port,runtime.render)) return 125;
    runtime.image=trusted_image(WDDM_SESSION_WRAPPER);runtime.broker_image=trusted_image(WDDM_SESSION_BROKER);
    if(runtime.image<0 || runtime.broker_image<0 || sigprocmask(SIG_BLOCK,&signals,NULL)) goto cleanup;
    runtime.stop=signalfd(-1,&signals,SFD_CLOEXEC|SFD_NONBLOCK);if(runtime.stop<0) goto cleanup;
#ifdef WDDM_SESSION_TEST_CGROUP
    const int group_length=snprintf(runtime.cgroup,sizeof runtime.cgroup,"%s",WDDM_SESSION_TEST_CGROUP);
#else
    const int group_length=snprintf(runtime.cgroup,sizeof runtime.cgroup,"/user.slice/user-%u.slice/user@%u.service/session.slice/wayland-wm@hyprland.desktop.service",(unsigned)runtime.owner.uid,(unsigned)runtime.owner.uid);
#endif
    if(group_length<=0 || (size_t)group_length>=sizeof runtime.cgroup || mkdir(WDDM_SESSION_ROOT,0755)) goto cleanup;
    runtime.directory_created=1;
    if(chmod(WDDM_SESSION_ROOT,0755)) goto cleanup;
    if(write_environment(&runtime)) goto cleanup;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(strlen(WDDM_SESSION_ROOT "/session.sock")>=sizeof address.sun_path) goto cleanup;
    strcpy(address.sun_path,WDDM_SESSION_ROOT "/session.sock");
    runtime.listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(runtime.listener<0) goto cleanup;
    if(bind(runtime.listener,(struct sockaddr*)&address,sizeof address)) goto cleanup;
    runtime.socket_created=1;
    if(lstat(address.sun_path,&runtime.socket_identity) || chown(address.sun_path,0,runtime.owner.gid) || chmod(address.sun_path,0660) || listen(runtime.listener,4) || root_notify()) goto cleanup;
    for(;;) {
        struct pollfd items[2]={{runtime.listener,POLLIN,0},{runtime.stop,POLLIN,0}};
        const int count=poll(items,2,-1);
        if(count<0 && errno==EINTR) continue;
        if(count<0) goto cleanup;
        if(items[1].revents) { result=0;goto cleanup; }
        if(!(items[0].revents&POLLIN)) goto cleanup;
        const int connection=accept4(runtime.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        if(connection<0) { if(errno==EAGAIN || errno==EINTR) continue;goto cleanup; }
        const uint64_t start=wddm_activation_now();
        const int pinned=wddm_session_pin(connection,runtime.owner.uid,runtime.owner.gid,runtime.image,runtime.cgroup,&peer);
        if(!pinned && !wddm_session_receive(connection,&peer,runtime.stop,start+500000000ull)) {
            const uint64_t deadline=start+9000000000ull;
            const int active=!broker_start(&runtime,&peer,deadline) && !wddm_session_reply(connection,&peer,runtime.stop,deadline);
            close(connection);
            if(!active) goto cleanup;
            struct pollfd state[3]={{peer.process,POLLIN,0},{runtime.broker_process,POLLIN,0},{runtime.stop,POLLIN,0}};
            for(;;) {
                const int ready=poll(state,3,-1);
                if(ready<0 && errno==EINTR) continue;
                if(ready<0 || (state[1].revents && !state[0].revents)) goto cleanup;
                if(state[2].revents) { if(stop_peer(&peer)) goto cleanup;break; }
                if(state[0].revents) break;
            }
            if(broker_reap(&runtime,0)) goto cleanup;
            result=0;goto cleanup;
        }
        close(connection);
        if(peer.process>=0) { close(peer.process);peer.process=-1; }
    }
cleanup:
    if(result && peer.process>=0) stop_peer(&peer);
    if(runtime.broker>0 && broker_reap(&runtime,1)) result=125;
    if(peer.process>=0) close(peer.process);
    if(runtime.listener>=0) close(runtime.listener);
    if(runtime.stop>=0) close(runtime.stop);
    if(runtime.image>=0) close(runtime.image);
    if(runtime.broker_image>=0) close(runtime.broker_image);
    if(runtime.environment_created && unlink_owned(WDDM_SESSION_ROOT "/compositor.env",&runtime.environment_identity)) result=125;
    if(runtime.socket_created && unlink_owned(WDDM_SESSION_ROOT "/session.sock",&runtime.socket_identity)) result=125;
    if(runtime.directory_created && rmdir(WDDM_SESSION_ROOT)) result=125;
    // The broker owns its socket. Remove only the root-created directory after
    // the child is reaped; do not recursively remove or unload GPU resources.
    if(runtime.broker_directory_created && runtime.broker<=0) {
        struct stat socket_state;
        if(!lstat(WDDM_SESSION_BROKER_DIRECTORY "/graphics.sock",&socket_state)) {
            if(!S_ISSOCK(socket_state.st_mode) || socket_state.st_uid!=runtime.owner.uid || unlink(WDDM_SESSION_BROKER_DIRECTORY "/graphics.sock")) result=125;
        } else if(errno!=ENOENT) result=125;
        if(rmdir(WDDM_SESSION_BROKER_DIRECTORY)) result=125;
    }
    return result;
}
