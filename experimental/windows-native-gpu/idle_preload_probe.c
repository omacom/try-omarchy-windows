// SPDX-License-Identifier: MIT
// Real libc/FD activity under LD_PRELOAD; no GPU endpoint or worker is selected.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"IDLE_PRELOAD_FAILED line=%d errno=%d\n",__LINE__,errno);exit(1); } } while(0)
static pthread_barrier_t ready;
static unsigned descriptors(void) {
    DIR* directory=opendir("/proc/self/fd");CHECK(directory);unsigned count=0;
    for(struct dirent* entry;(entry=readdir(directory));) if(entry->d_name[0]!='.') ++count;
    CHECK(!closedir(directory));return count;
}
static void ordinary(void) {
    int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    errno=E2BIG;CHECK(!close(fd) && errno==E2BIG);
    int pair[2];CHECK(!pipe2(pair,O_CLOEXEC));struct winsize size={0};
    errno=0;CHECK(ioctl(pair[0],TIOCGWINSZ,&size)==-1 && errno==ENOTTY);
    CHECK(!close(pair[0]) && !close(pair[1]));
}
static void rendezvous(void) {
    int result=pthread_barrier_wait(&ready);CHECK(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);
}
static void* idle_worker(void* unused) {
    (void)unused;rendezvous();for(unsigned n=0;n<256;++n) ordinary();return NULL;
}
static void* failed_open(void* unused) {
    (void)unused;rendezvous();errno=0;
    CHECK(open("/dev/dxg",O_RDWR|O_CLOEXEC)==-1 && errno==EINVAL);return NULL;
}
int main(int argc,char** argv) {
    CHECK(argc==2);alarm(15);unsigned before=descriptors();
    const int race=!strcmp(argv[1],"race"),failure=!strcmp(argv[1],"failure");
    CHECK(race || failure || !strcmp(argv[1],"idle"));
    if(failure) { errno=0;CHECK(open("/dev/dxg",O_RDWR|O_CLOEXEC)==-1 && errno==EINVAL);ordinary(); }
    else {
        CHECK(!pthread_barrier_init(&ready,NULL,race?5:4));pthread_t workers[5];
        for(unsigned n=0;n<4;++n) CHECK(!pthread_create(&workers[n],NULL,idle_worker,NULL));
        if(race) CHECK(!pthread_create(&workers[4],NULL,failed_open,NULL));
        for(unsigned n=0;n<(race?5u:4u);++n) CHECK(!pthread_join(workers[n],NULL));
        CHECK(!pthread_barrier_destroy(&ready));
    }
    CHECK(descriptors()==before);alarm(0);
    printf("IDLE_PRELOAD_CONTROL mode=%s descriptorsBalanced=true closeErrnoPreserved=true realIoctlForwarded=true gpu=false\n",argv[1]);return 0;
}
