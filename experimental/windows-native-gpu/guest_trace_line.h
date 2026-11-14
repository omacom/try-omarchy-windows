/* SPDX-License-Identifier: MIT */
/* One bounded FIFO write; a partial journal prefix cannot swallow the receipt. */
#pragma once
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int guest_trace_line(const char* format,...) {
    char line[512];line[0]='\n';
    va_list args;va_start(args,format);
    const int bytes=vsnprintf(line+1,sizeof(line)-1,format,args);
    va_end(args);
    if(bytes<=0 || (size_t)bytes>=sizeof(line)-1 || line[bytes]!='\n' ||
       memchr(line+1,'\n',(size_t)bytes-1)) { errno=EMSGSIZE;return -1; }
    ssize_t written;
    do { written=write(STDOUT_FILENO,line,(size_t)bytes+1); } while(written<0 && errno==EINTR);
    if(written<0) return -1;
    if(written!=bytes+1) { errno=EIO;return -1; }
    return 0;
}
