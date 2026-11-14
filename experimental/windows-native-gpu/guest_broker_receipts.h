// SPDX-License-Identifier: MIT
// Diagnostic receipts share a tty with unrelated guest traces. Bound and emit
// each complete record in one write, with an explicit leading line boundary.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
namespace guest_broker {
inline bool receiptV(int fd,const char* format,va_list arguments) noexcept {
    char line[2048];line[0]='\n';
    const int count=std::vsnprintf(line+1,sizeof(line)-2,format,arguments);
    if(count<=0 || static_cast<std::size_t>(count)>=sizeof(line)-2) return false;
    auto bytes=static_cast<std::size_t>(count)+1;
    if(line[bytes-1]!='\n') line[bytes++]='\n';
    ssize_t written;
    do { written=write(fd,line,bytes); } while(written<0 && errno==EINTR);
    return written==static_cast<ssize_t>(bytes);
}
__attribute__((format(printf,2,3))) inline bool receiptTo(int fd,const char* format,...) noexcept {
    va_list arguments;va_start(arguments,format);const bool result=receiptV(fd,format,arguments);va_end(arguments);return result;
}
__attribute__((format(printf,1,2))) inline bool receipt(const char* format,...) noexcept {
    va_list arguments;va_start(arguments,format);const bool result=receiptV(STDERR_FILENO,format,arguments);va_end(arguments);return result;
}
}
