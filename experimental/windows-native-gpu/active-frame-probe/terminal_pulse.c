/* SPDX-License-Identifier: MIT */
/* Real terminal updates at an absolute 60 Hz schedule; no GPU/input emulation. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) { (void)signal_number;stopped=1; }
static uint64_t monotonic_ns(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
static int write_all(int fd,const char* bytes,size_t count) {
    while(count) {
        const ssize_t written=write(fd,bytes,count);
        if(written<0 && errno==EINTR) { if(stopped) return -1;continue; }
        if(written<=0) return -1;
        bytes+=written;count-=(size_t)written;
    }
    return 0;
}
static int receipt(uint64_t started,uint64_t last,unsigned updates,uint64_t skipped,uint64_t late) {
    char path[128],body[512];
    const int length=snprintf(path,sizeof path,"/tmp/omarchy-active-terminal-%ld.tmp",(long)getpid());
    const int count=snprintf(body,sizeof body,
        "{\"pid\":%ld,\"parentPid\":%ld,\"uid\":%ld,\"updates\":%u,\"targetHz\":60,\"startedNs\":%" PRIu64
        ",\"lastNs\":%" PRIu64 ",\"skippedSlots\":%" PRIu64 ",\"maximumLateNs\":%" PRIu64 ",\"gpuTimestamp\":false}\n",
        (long)getpid(),(long)getppid(),(long)getuid(),updates,started,last,skipped,late);
    if(length<=0 || (size_t)length>=sizeof path || count<=0 || (size_t)count>=sizeof body) return -1;
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    const int status=write_all(fd,body,(size_t)count),closed=close(fd);
    if(status || closed) { unlink(path);return -1; }
    if(rename(path,"/tmp/omarchy-active-terminal-pulse.json")) { unlink(path);return -1; }
    return 0;
}
int main(void) {
    struct sigaction action={0};action.sa_handler=stop;
    if(sigemptyset(&action.sa_mask) || sigaction(SIGHUP,&action,NULL) || sigaction(SIGTERM,&action,NULL) ||
       sigaction(SIGINT,&action,NULL)) return 2;
    const uint64_t period=16666667ull,started=monotonic_ns();
    if(!started) return 2;
    uint64_t next=started,last=started,skipped=0,maximum_late=0;
    unsigned updates=0;
    while(!stopped && updates<3600) {
        struct timespec deadline={(time_t)(next/1000000000ull),(long)(next%1000000000ull)};
        int status;
        do { status=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL); } while(status==EINTR && !stopped);
        if(stopped) break;
        if(status) return 3;
        last=monotonic_ns();if(!last || last<next) return 3;
        const uint64_t late=last-next;
        if(late>maximum_late) maximum_late=late;
        char text[160];
        const int count=snprintf(text,sizeof text,"\r\033[KActive application updates: %06u  \033[48;2;%u;80;160m        \033[0m",updates+1,(updates*7u)%256u);
        if(count<=0 || (size_t)count>=sizeof text || write_all(STDOUT_FILENO,text,(size_t)count)) break;
        ++updates;
        if(updates==1 || updates%60u==0) { if(receipt(started,last,updates,skipped,maximum_late)) return 4; }
        // A late producer skips missed slots instead of flooding catch-up writes.
        const uint64_t missed=late/period;skipped+=missed;next+=(missed+1)*period;
    }
    return receipt(started,last,updates,skipped,maximum_late)?4:0;
}
