// SPDX-License-Identifier: MIT
// Compositor-only exec target for start-hyprland --path; no fork or PID change.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include "hyprland_session_invocation.h"
#include "guest_session_activation.h"
#ifndef WDDM_SESSION_EXEC_TARGET
#define WDDM_SESSION_EXEC_TARGET "/usr/bin/Hyprland"
#endif
#ifndef WDDM_SESSION_ENVIRONMENT
#define WDDM_SESSION_ENVIRONMENT "/run/omarchy-nvgpu/compositor.env"
#endif
#ifndef WDDM_SESSION_ACTIVATION_SOCKET
#define WDDM_SESSION_ACTIVATION_SOCKET "/run/omarchy-nvgpu/session.sock"
#endif
static const char* const keys[]={"LD_PRELOAD","LD_LIBRARY_PATH","D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE",
    "D3D12_EXPERIMENTAL_NATIVE_BUFFERS","WDDM_BRIDGE_COMPOSITOR_SESSION","GALLIUM_DRIVER","LIBGL_DRIVERS_PATH","WDDM_BRIDGE_KERNEL_APERTURE",
    "WDDM_BRIDGE_RENDER_NODE","WDDM_BRIDGE_BROKER_SOCKET","WDDM_BRIDGE_LINUX_UMD_NAME",
    "AQ_EXPERIMENTAL_NATIVE_RENDER","WDDM_BRIDGE_NATIVE_INPUT","WDDM_BRIDGE_SESSION_SIZE",
    "MESA_D3D12_DEFAULT_ADAPTER_NAME","WDDM_BRIDGE_ACTOR_PROFILE","GBM_BACKENDS_PATH"};
struct environment { char data[16384];char* values[sizeof keys/sizeof keys[0]]; };
static int owner_number(const char* text,unsigned actual) {
    if(!*text || *text=='0') return -1;
    uint64_t value=0;
    for(;*text;++text) {
        if(*text<'0' || *text>'9' || value>UINT32_MAX/10u) return -1;
        value=value*10+(unsigned)(*text-'0');
        if(value>=UINT32_MAX) return -1;
    }
    return value==actual?0:-1;
}
static int read_environment(struct environment* env) {
    memset(env,0,sizeof *env);
    const int fd=open(WDDM_SESSION_ENVIRONMENT,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) return -1;
    struct stat state;
    int result=-1;
    if(fstat(fd,&state) || !S_ISREG(state.st_mode) || state.st_uid || (state.st_mode&0022) ||
       state.st_nlink!=1 || state.st_size<=0 || state.st_size>=(off_t)sizeof env->data) goto done;
    size_t used=0;
    while(used<sizeof env->data-1) {
        const ssize_t count=read(fd,env->data+used,sizeof env->data-1-used);
        if(count<0 && errno==EINTR) continue;
        if(count<0) goto done;
        if(!count) break;
        used+=(size_t)count;
    }
    if(used!=(size_t)state.st_size || used>=sizeof env->data-1 || env->data[used-1]!='\n' || memchr(env->data,0,used)) goto done;
    char* cursor=env->data;const char* end=env->data+used;
    unsigned owner_fields=0;
    while(cursor<end) {
        char* line=strchr(cursor,'\n');
        if(!line || line-cursor>2048) goto done;
        *line=0;char* equals=strchr(cursor,'=');
        if(!equals || equals==cursor) goto done;
        *equals=0;char* value=equals+1;
        if(!strcmp(cursor,"WDDM_SESSION_VERSION")) {
            if((owner_fields&1) || strcmp(value,"1")) goto done;
            owner_fields|=1;
        } else if(!strcmp(cursor,"WDDM_SESSION_UID")) {
            if((owner_fields&2) || owner_number(value,(unsigned)getuid())) goto done;
            owner_fields|=2;
        } else if(!strcmp(cursor,"WDDM_SESSION_GID")) {
            if((owner_fields&4) || owner_number(value,(unsigned)getgid())) goto done;
            owner_fields|=4;
        } else {
            size_t index=0;
            while(index<sizeof keys/sizeof keys[0] && strcmp(keys[index],cursor)) ++index;
            if(index==sizeof keys/sizeof keys[0] || env->values[index] || line-value<3 ||
               value[0]!='"' || line[-1]!='"') goto done;
            line[-1]=0;++value;
            if(!*value) goto done;
            for(const unsigned char* p=(const unsigned char*)value;*p;++p)
                if(*p<32 || *p>126 || *p=='"' || *p=='\\') goto done;
            env->values[index]=value;
        }
        cursor=line+1;
    }
    if(owner_fields!=7 || !env->values[0] || !env->values[1] || !env->values[2] || !env->values[3] || !env->values[4] ||
       strcmp(env->values[2],"true") || strcmp(env->values[3],"true") || strcmp(env->values[4],"1")) goto done;
    result=0;
done:
    if(close(fd)) result=-1;
    return result;
}
static int activate(void) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    const size_t length=strlen(WDDM_SESSION_ACTIVATION_SOCKET);
    if(length>=sizeof address.sun_path) return -1;
    memcpy(address.sun_path,WDDM_SESSION_ACTIVATION_SOCKET,length+1);
    const int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0) return -1;
    const uint64_t start=wddm_activation_now();
    if(!start || start>UINT64_MAX-10000000000ull) { close(fd);return -1; }
    const uint64_t deadline=start+10000000000ull;
    int result=connect(fd,(struct sockaddr*)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+length+1));
    if(result && errno==EINPROGRESS) {
        if(!wddm_activation_wait(fd,-1,POLLOUT,deadline)) {
            int error=0;socklen_t bytes=sizeof error;
            result=getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&bytes) || bytes!=sizeof error || error;
        }
    }
    if(!result) result=wddm_activation_exchange_until(fd,deadline);
    if(close(fd)) result=-1;
    return result;
}
int main(int argc,char** argv) {
    if(!wddm_hyprland_renders(argc,argv)) {
        argv[0]=WDDM_SESSION_EXEC_TARGET;execv(WDDM_SESSION_EXEC_TARGET,argv);return 127;
    }
    if(!getuid() || getuid()!=geteuid() || getgid()!=getegid()) return 125;
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2]={{0}};
    if(syscall(SYS_capget,&header,caps) || caps[0].effective || caps[1].effective || caps[0].permitted || caps[1].permitted) return 125;
    struct environment env;
    if(read_environment(&env) || activate()) return 125;
    for(size_t index=0;index<sizeof keys/sizeof keys[0];++index)
        if(env.values[index] && setenv(keys[index],env.values[index],1)) return 125;
    if(unsetenv("EGL_PLATFORM") || unsetenv("WDDM_BRIDGE_PORT") || unsetenv("WDDM_BRIDGE_WINDOWS_WORKER")) return 125;
    argv[0]=WDDM_SESSION_EXEC_TARGET;
    execv(WDDM_SESSION_EXEC_TARGET,argv);return 127;
}
