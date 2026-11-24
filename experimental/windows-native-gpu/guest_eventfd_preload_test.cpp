// SPDX-License-Identifier: MIT
// Calls the actual libc API under each preload profile, with no bridge/device.
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
static void require(bool value) { if (!value) std::abort(); }
int main(int argc, char** argv) {
    require(argc == 2);
    const bool normalized = std::atoi(argv[1]) == 1;
    const int saved = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3); require(saved >= 3);
    require(syscall(SYS_close, STDIN_FILENO) == 0);
    const int fd = eventfd(2, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
    require(normalized ? fd >= 3 : fd == 0);
    if (normalized) { errno = 0; require(fcntl(0, F_GETFD) == -1 && errno == EBADF); }
    eventfd_t value = 0;
    require((fcntl(fd, F_GETFD) & FD_CLOEXEC) && eventfd_read(fd, &value) == 0 && value == 1);
    require(eventfd_read(fd, &value) == 0 && value == 1);
    require(eventfd_read(fd, &value) == -1 && errno == EAGAIN);
    require(syscall(SYS_close, fd) == 0);
    require(dup2(saved, STDIN_FILENO) == STDIN_FILENO); syscall(SYS_close, saved);
    std::printf("EVENTFD_PRELOAD_PASS normalized=%u counterAndFlagsPreserved=true gpu=false\n", normalized ? 1u : 0u);
}
