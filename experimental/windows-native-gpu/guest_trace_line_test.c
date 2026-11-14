/* SPDX-License-Identifier: MIT */
/* Actual pipes/processes: prefix collision, concurrent writers and write faults. */
#define _GNU_SOURCE
#include "guest_trace_line.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/wait.h>
static void exited(pid_t child) {
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(void) {
    int pipefd[2];assert(!pipe(pipefd));
    pid_t legacy=fork();assert(legacy>=0);
    if(!legacy) {
        close(pipefd[0]);assert(dup2(pipefd[1],STDOUT_FILENO)>=0);close(pipefd[1]);
        assert(write(STDOUT_FILENO,"partial-journal-prefix",22)==22);
        assert(dprintf(STDOUT_FILENO,"GUEST_TRACE_TEST index=0 gpu=false\n")>0);_exit(0);
    }
    close(pipefd[1]);char legacy_bytes[256]={0};size_t used=0;ssize_t count;
    while((count=read(pipefd[0],legacy_bytes+used,sizeof(legacy_bytes)-used-1))>0) used+=(size_t)count;
    assert(!count);close(pipefd[0]);exited(legacy);
    assert(strstr(legacy_bytes,"partial-journal-prefixGUEST_TRACE_TEST") && strncmp(legacy_bytes,"GUEST_TRACE_TEST",16));
    assert(!pipe(pipefd));
    pid_t journal=fork();assert(journal>=0);
    if(!journal) {
        close(pipefd[0]);
        for(unsigned n=0;n<2000;++n) assert(write(pipefd[1],"partial-journal-prefix",22)==22);
        close(pipefd[1]);_exit(0);
    }
    pid_t renderer=fork();assert(renderer>=0);
    if(!renderer) {
        close(pipefd[0]);assert(dup2(pipefd[1],STDOUT_FILENO)>=0);close(pipefd[1]);
        assert(guest_trace_line("no terminator")==-1 && errno==EMSGSIZE);
        assert(guest_trace_line("two\nlines\n")==-1 && errno==EMSGSIZE);
        assert(guest_trace_line("%0600u\n",1)==-1 && errno==EMSGSIZE);
        for(unsigned n=0;n<2000;++n) assert(!guest_trace_line("GUEST_TRACE_TEST index=%u gpu=false\n",n));
        close(STDOUT_FILENO);assert(guest_trace_line("closed\n")==-1 && errno==EBADF);_exit(0);
    }
    close(pipefd[1]);char* bytes=calloc(1,1024*1024);assert(bytes);used=0;
    while((count=read(pipefd[0],bytes+used,1024*1024-used-1))>0) { used+=(size_t)count;assert(used<1024*1024-1); }
    assert(!count);close(pipefd[0]);exited(journal);exited(renderer);
    unsigned receipts=0;char* state=NULL;
    for(char* line=strtok_r(bytes,"\n",&state);line;line=strtok_r(NULL,"\n",&state)) {
        if(!strncmp(line,"GUEST_TRACE_TEST ",17)) {
            unsigned index=UINT_MAX;char tail=0;
            assert(sscanf(line,"GUEST_TRACE_TEST index=%u gpu=false%c",&index,&tail)==1 && index==receipts++);
        } else assert(!strstr(line,"GUEST_TRACE_TEST"));
    }
    free(bytes);assert(receipts==2000);
    puts("GUEST_TRACE_LINE legacyPrefixCollisionReproduced=true atomicReceipts=2000 concurrentWriter=true boundedSingleLine=true writeErrorsPropagated=true gpu=false");
}
