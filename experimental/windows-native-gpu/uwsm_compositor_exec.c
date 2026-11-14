// SPDX-License-Identifier: MIT
// Apply the owned diagnostic GPU environment only at the compositor exec,
// leaving UWSM's Python process and its environment preparation on OS libraries.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "guest_graphics_port.h"

static int compositor_environment(char* data,size_t bytes) {
    if(!bytes || data[bytes-1]!='\n' || memchr(data,0,bytes)) return -1;
    char* cursor=data;char* end=data+bytes;unsigned entries=0;
    while(cursor<end) {
        char* line=memchr(cursor,'\n',(size_t)(end-cursor));
        if(!line || ++entries>64 || line-cursor>1024) return -1;
        *line=0;
        char* equals=strchr(cursor,'=');
        if(!equals || equals==cursor || line-equals<3 || equals[1]!='"' || line[-1]!='"') return -1;
        *equals=0;
        if(strspn(cursor,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=strlen(cursor) ||
           (cursor[0]>='0' && cursor[0]<='9')) return -1;
        line[-1]=0;
        if(strpbrk(equals+2,"\r\"\\") || setenv(cursor,equals+2,1)) return -1;
        cursor=line+1;
    }
    return 0;
}
int main(int argc,char** argv) {
    (void)argv;
    if(argc!=1 || getuid()!=1000 || geteuid()!=1000) return 125;
    const char* session=getenv("XDG_SESSION_ID");
    if(!session || !*session || strlen(session)>64 || strspn(session,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=strlen(session)) return 125;
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct capabilities[2]={{0}};
    if(syscall(SYS_capget,&header,capabilities) || capabilities[0].effective || capabilities[1].effective ||
       prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1) return 125;
    char graphics_port[128];
    if(wddm_graphics_port_resolve("/sys/class/virtio-ports","/dev",graphics_port,sizeof graphics_port)) {
        perror("named graphics port for compositor");return 125;
    }
    const char* privileged[]={"/dev/wddm-aperture",graphics_port};
    for(unsigned index=0;index<2;++index) {
        int probe=open(privileged[index],O_RDWR|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
        if(probe>=0) { close(probe);return 125; }
        if(errno!=EPERM && errno!=EACCES) return 125;
    }
    printf("BROKER_RENDER_CLIENT uid=1000 gid=%u effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied\n",(unsigned)getgid());
    printf("UWSM_COMPOSITOR_LOGIN pid=%ld uid=1000 sessionId=%s realPamSession=true gpuProof=false\n",(long)getpid(),session);
    fflush(stdout);
    int fd=open("/run/wddm-broker/compositor.env",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    struct stat file;
    if(fd<0) return 125;
    if(fstat(fd,&file) || !S_ISREG(file.st_mode) || file.st_uid || (file.st_mode&0022) ||
       file.st_nlink!=1 || file.st_size<=0 || file.st_size>=16384) { close(fd);return 125; }
    char data[16384];size_t bytes=0;
    for(;;) {
        ssize_t count=read(fd,data+bytes,sizeof data-bytes);
        if(count<0 && errno==EINTR) continue;
        if(count<0 || bytes+(size_t)count>=sizeof data) { close(fd);return 125; }
        if(!count) break;
        bytes+=(size_t)count;
    }
    close(fd);
    if(bytes!=(size_t)file.st_size || compositor_environment(data,bytes) ||
       unsetenv("EGL_PLATFORM") || unsetenv("WDDM_BRIDGE_PORT") || unsetenv("WDDM_BRIDGE_WINDOWS_WORKER")) return 125;
    execl("/guest-probe","/guest-probe",(char*)NULL);
    perror("UWSM GPU compositor exec");return 127;
}
