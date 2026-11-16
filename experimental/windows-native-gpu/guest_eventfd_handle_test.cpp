// SPDX-License-Identifier: MIT
// Real eventfd counters/flags, including zero and exhaustion; no GPU/fence stores.
#include "guest_eventfd_handle.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <vector>
static void require(bool value) { if (!value) std::abort(); }
static bool closed(int fd) { errno = 0; return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }
struct Stdin {
    int saved = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    Stdin() { require(saved >= 0); require(close(STDIN_FILENO) == 0); }
    ~Stdin() { require(dup2(saved, STDIN_FILENO) == STDIN_FILENO); close(saved); }
};
int main() {
    unsigned checks = 0;
    {
        Stdin input;
        const int fd = guest_eventfd::create(3, EFD_NONBLOCK | EFD_CLOEXEC, false);
        eventfd_t value = 0;
        require(fd == 0 && eventfd_read(fd, &value) == 0 && value == 3 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
        close(fd); ++checks;
    }
    {
        Stdin input;
        const int fd = guest_eventfd::create(3, EFD_NONBLOCK | EFD_CLOEXEC, true);
        eventfd_t value = 0;
        require(fd >= 3 && closed(0) && eventfd_read(fd, &value) == 0 && value == 3 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
        require(eventfd_read(fd, &value) == -1 && errno == EAGAIN);
        require(eventfd_write(fd, 7) == 0 && eventfd_read(fd, &value) == 0 && value == 7);
        close(fd); ++checks;
    }
    {
        Stdin input;
        const int fd = guest_eventfd::create(2, EFD_NONBLOCK | EFD_SEMAPHORE, true);
        eventfd_t value = 0;
        require(fd >= 3 && !(fcntl(fd, F_GETFD) & FD_CLOEXEC));
        require(eventfd_read(fd, &value) == 0 && value == 1 && eventfd_read(fd, &value) == 0 && value == 1);
        require(eventfd_read(fd, &value) == -1 && errno == EAGAIN); close(fd); ++checks;
    }
    {
        const int expected = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3); require(expected >= 3); close(expected);
        const int fd = guest_eventfd::create(0, EFD_NONBLOCK, true);
        require(fd == expected && !(fcntl(fd, F_GETFD) & FD_CLOEXEC)); close(fd); ++checks;
    }
    {
        Stdin input;
        errno = 0; require(guest_eventfd::create(0, 0x7fffffff, true) == -1 && errno == EINVAL && closed(0)); ++checks;
    }
    {
        Stdin input;
        struct rlimit previous{}; require(getrlimit(RLIMIT_NOFILE, &previous) == 0);
        auto bounded = previous; bounded.rlim_cur = std::min<rlim_t>(previous.rlim_cur, 64);
        require(bounded.rlim_cur > 8 && setrlimit(RLIMIT_NOFILE, &bounded) == 0);
        std::vector<int> occupied;
        for (;;) { const int fd = fcntl(input.saved, F_DUPFD_CLOEXEC, 3); if (fd < 0) { require(errno == EMFILE); break; } occupied.push_back(fd); }
        errno = 0; const int fd = guest_eventfd::create(0, EFD_NONBLOCK | EFD_CLOEXEC, true);
        const int error = errno;
        require(fd == -1 && error == EMFILE && closed(0));
        for (const int descriptor : occupied) close(descriptor);
        require(setrlimit(RLIMIT_NOFILE, &previous) == 0); ++checks;
    }
    std::printf("EVENTFD_HANDLE_CONTRACT_PASS checks=%u counterAndFlagsPreserved=true exhaustionRollback=true gpu=false\n", checks);
}
