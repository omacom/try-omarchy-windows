/* SPDX-License-Identifier: MIT */
/* Ordered, bounded, live log copy. Standard input is never changed. */
#pragma once
#include "trace_port.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>

#define TRACE_FIFO "/run/wddm-graphics-trace/session.fifo"
#define TRACE_MAX_BYTES (64u*1024u*1024u)
static pid_t trace_child=-1;
static int trace_saved_out=-1,trace_saved_error=-1;

static int trace_write_all(int fd,const void* data,size_t bytes) {
    const unsigned char* p=data;
    while(bytes) {
        const ssize_t n=write(fd,p,bytes);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return -1;
        p+=n;bytes-=(size_t)n;
    }
    return 0;
}
static int trace_pump_fd(int input,int output,uint64_t limit) {
    char framing[160];unsigned char data[65536];
    uint64_t bytes=0,hash=UINT64_C(14695981039346656037);
    int size=snprintf(framing,sizeof framing,"WDDM_VIRTIO_TRACE_BEGIN version=1 maxBytes=%llu\n",(unsigned long long)limit);
    if(size<=0 || (size_t)size>=sizeof framing || trace_write_all(output,framing,(size_t)size)) return 125;
    for(;;) {
        const ssize_t count=read(input,data,sizeof data);
        if(count<0 && errno==EINTR) continue;
        if(count<0) return 125;
        if(!count) break;
        if(bytes>limit || (uint64_t)count>limit-bytes || trace_write_all(output,data,(size_t)count)) return 125;
        bytes+=(uint64_t)count;
        for(ssize_t i=0;i<count;++i) hash=(hash^data[i])*UINT64_C(1099511628211);
    }
    size=snprintf(framing,sizeof framing,"WDDM_VIRTIO_TRACE_END bytes=%llu fnv1a=%016llx complete=true\n",
                  (unsigned long long)bytes,(unsigned long long)hash);
    return size<=0 || (size_t)size>=sizeof framing || trace_write_all(output,framing,(size_t)size) ? 125 : 0;
}
static int trace_session_end(void) {
    if(trace_child<=0) return 0;
    const int restored_out=dup2(trace_saved_out,STDOUT_FILENO)>=0;
    const int restored_error=dup2(trace_saved_error,STDERR_FILENO)>=0;
    const int closed_out=!close(trace_saved_out),closed_error=!close(trace_saved_error);
    int ok=restored_out && restored_error && closed_out && closed_error;
    trace_saved_out=trace_saved_error=-1;
    int status=0,done=0;
    for(unsigned tick=0;tick<500;++tick) {
        const pid_t result=waitpid(trace_child,&status,WNOHANG);
        if(result==trace_child) { done=1;break; }
        if(result<0 && errno!=EINTR) break;
        usleep(10000);
    }
    if(!done) {
        kill(trace_child,SIGKILL);
        for(unsigned tick=0;tick<100;++tick) {
            if(waitpid(trace_child,&status,WNOHANG)==trace_child) { done=1;break; }
            usleep(10000);
        }
        ok=0;
    }
    ok=ok && done && WIFEXITED(status) && !WEXITSTATUS(status);
    trace_child=-1;
    printf("NORMAL_ROOT_VIRTIO_TRACE_END pumpExited=%s complete=%s inputChanged=false\n",done?"true":"false",ok?"true":"false");
    return ok?0:-1;
}
static void trace_exit_cleanup(void) { if(trace_child>0) (void)trace_session_end(); }

static int trace_session_begin(unsigned uid,unsigned gid) {
    if(trace_child>0 || mkdir("/run/wddm-graphics-trace",0710) ||
       chown("/run/wddm-graphics-trace",0,gid) || chmod("/run/wddm-graphics-trace",0710) ||
       mkfifo(TRACE_FIFO,0620) || chown(TRACE_FIFO,0,gid) || chmod(TRACE_FIFO,0620)) return -1;
    const int port=trace_port_open();if(port<0) return -1;
    const int input=open(TRACE_FIFO,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
    const int writer=input<0 ? -1 : open(TRACE_FIFO,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(input<0 || writer<0 || fcntl(input,F_SETFL,0)) { if(input>=0) close(input);if(writer>=0) close(writer);close(port);return -1; }
    trace_saved_out=fcntl(STDOUT_FILENO,F_DUPFD_CLOEXEC,3);
    trace_saved_error=fcntl(STDERR_FILENO,F_DUPFD_CLOEXEC,3);
    if(trace_saved_out<0 || trace_saved_error<0) { close(input);close(writer);close(port);return -1; }
    printf("NORMAL_ROOT_VIRTIO_TRACE_BEGIN port=vport0p2 namedIdentityVerified=true liveHostCapture=true inputChanged=false gpuProtocolChanged=false uid=%u\n",uid);
    trace_child=fork();
    if(!trace_child) {
        close(writer);close(trace_saved_out);close(trace_saved_error);
        if(prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()==1) _exit(125);
        _exit(trace_pump_fd(input,port,TRACE_MAX_BYTES));
    }
    close(input);close(port);
    if(trace_child<0) { close(writer);return -1; }
    if(atexit(trace_exit_cleanup) || dup2(writer,STDOUT_FILENO)<0 || dup2(writer,STDERR_FILENO)<0) { close(writer);return -1; }
    return close(writer);
}
