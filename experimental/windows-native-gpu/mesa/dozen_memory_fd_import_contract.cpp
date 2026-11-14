// SPDX-License-Identifier: MIT
// Actual Linux FD ownership; no Vulkan device or GPU is created.
#include <vulkan/vulkan_core.h>
#include "util/os_file.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "duplicate_helper.inc"
#include "dzn_imported_memory_fd.h"

static unsigned cases = 0, failures = 0;
static void setup(bool value) { if (!value) { std::perror("DOZEN_IMPORT_FD_SETUP_FAIL"); std::abort(); } }
static void check(const char* name, bool passed) {
    ++cases; if (!passed) ++failures;
    std::printf("DOZEN_IMPORT_FD_CONTRACT case=%s passed=%s gpu=false\n", name, passed ? "true" : "false");
}
static int file() {
    const int fd = memfd_create("dozen-import-contract", MFD_CLOEXEC); setup(fd >= 0);
    setup(write(fd, "payload", 7) == 7); return fd;
}
static bool payload(int fd) { char bytes[8]{}; return pread(fd, bytes, 7, 0) == 7 && std::memcmp(bytes, "payload", 7) == 0; }
static bool closed(int fd) { errno = 0; return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }
static bool same(int a, int b) {
    struct stat x{}, y{}; return fstat(a, &x) == 0 && fstat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--exec-closed") == 0) return closed(std::atoi(argv[2])) ? 0 : 1;
    setup(argc == 1);
    {
        dzn_imported_memory_fd owner{-1, -1};
        const int bad = file(); close(bad);
        check("invalid-input", dzn_imported_memory_fd_open(&owner, -1) == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
            dzn_imported_memory_fd_open(&owner, bad) == VK_ERROR_INVALID_EXTERNAL_HANDLE && owner.source_fd == -1 && owner.duplicate_fd == -1);
    }
    {
        const int source = file(); dzn_imported_memory_fd owner{-1, -1};
        setup(dzn_imported_memory_fd_open(&owner, source) == VK_SUCCESS);
        const int duplicate = owner.duplicate_fd;
        const bool before = duplicate >= 3 && duplicate != source && same(source, duplicate) && payload(source) && payload(duplicate);
        dzn_imported_memory_fd_finish(&owner, true);
        check("commit-closes-both", before && closed(source) && closed(duplicate) && owner.source_fd == -1 && owner.duplicate_fd == -1);
    }
    {
        const int source = file(); dzn_imported_memory_fd owner{-1, -1};
        setup(dzn_imported_memory_fd_open(&owner, source) == VK_SUCCESS); const int duplicate = owner.duplicate_fd;
        dzn_imported_memory_fd_finish(&owner, false);
        check("rollback-retains-source", payload(source) && closed(duplicate) && owner.source_fd == -1 && owner.duplicate_fd == -1);
        close(source);
    }
    {
        const int saved = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3); setup(saved >= 0);
        const int source = file(); setup(dup2(source, STDIN_FILENO) == 0); close(source);
        dzn_imported_memory_fd owner{-1, -1};
        const auto result = dzn_imported_memory_fd_open(&owner, 0); const int duplicate = owner.duplicate_fd;
        const bool before = result == VK_SUCCESS && duplicate >= 3 && same(0, duplicate) && payload(duplicate);
        dzn_imported_memory_fd_finish(&owner, true);
        check("descriptor-zero", before && closed(0) && closed(duplicate));
        setup(dup2(saved, STDIN_FILENO) == 0); close(saved);
    }
    {
        const int source = file(); dzn_imported_memory_fd owner{-1, -1};
        struct rlimit previous{}; setup(getrlimit(RLIMIT_NOFILE, &previous) == 0);
        auto bounded = previous; bounded.rlim_cur = std::min<rlim_t>(previous.rlim_cur, 64);
        setup(bounded.rlim_cur > 8 && setrlimit(RLIMIT_NOFILE, &bounded) == 0);
        std::vector<int> descriptors;
        for (;;) { const int fd = fcntl(source, F_DUPFD_CLOEXEC, 3); if (fd < 0) { setup(errno == EMFILE); break; } descriptors.push_back(fd); }
        const auto result = dzn_imported_memory_fd_open(&owner, source);
        const bool valid = result == VK_ERROR_OUT_OF_HOST_MEMORY && owner.source_fd == -1 && owner.duplicate_fd == -1 && payload(source);
        for (const int fd : descriptors) close(fd);
        setup(setrlimit(RLIMIT_NOFILE, &previous) == 0); check("exhaustion-rollback", valid); close(source);
    }
    {
        const int source = file(); dzn_imported_memory_fd owner{-1, -1};
        setup(dzn_imported_memory_fd_open(&owner, source) == VK_SUCCESS); const int duplicate = owner.duplicate_fd;
        const bool flags = (fcntl(duplicate, F_GETFD) & FD_CLOEXEC) != 0;
        const auto child = fork(); setup(child >= 0);
        if (!child) {
            char value[32]; std::snprintf(value, sizeof value, "%d", duplicate);
            execl(argv[0], argv[0], "--exec-closed", value, static_cast<char*>(nullptr)); _exit(127);
        }
        int status = 0; setup(waitpid(child, &status, 0) == child);
        check("close-on-exec", flags && WIFEXITED(status) && WEXITSTATUS(status) == 0 && payload(source));
        dzn_imported_memory_fd_finish(&owner, true);
    }
    {
        const int source = file(); dzn_imported_memory_fd owner{-1, -1};
        setup(dzn_imported_memory_fd_open(&owner, source) == VK_SUCCESS); const int duplicate = owner.duplicate_fd;
        dzn_imported_memory_fd_finish(&owner, false);
        const int temporary = file(); setup(dup2(temporary, duplicate) == duplicate); if (temporary != duplicate) close(temporary);
        dzn_imported_memory_fd_finish(&owner, false); dzn_imported_memory_fd_finish(&owner, true);
        check("idempotent-cleanup-reuse", payload(source) && payload(duplicate)); close(source); close(duplicate);
    }
    {
        const int source = file(), other = file(); dzn_imported_memory_fd owner{-1, -1};
        setup(dzn_imported_memory_fd_open(&owner, source) == VK_SUCCESS); const int duplicate = owner.duplicate_fd;
        check("busy-owner-rollback", dzn_imported_memory_fd_open(&owner, other) == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
            owner.source_fd == source && owner.duplicate_fd == duplicate && same(source, duplicate) && payload(other));
        dzn_imported_memory_fd_finish(&owner, false); close(source); close(other);
    }
    std::printf("DOZEN_IMPORT_FD_CONTRACT_COMPLETE cases=%u failed=%u gpu=false\n", cases, failures);
    return failures ? 1 : 0;
}
