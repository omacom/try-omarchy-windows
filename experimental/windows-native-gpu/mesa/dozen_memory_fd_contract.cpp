// SPDX-License-Identifier: MIT
// Exact extracted Dozen function + Mesa dup helper; real Linux descriptors.
// The small memory/device adapter below is a CPU contract, never GPU evidence.
#include <vulkan/vulkan.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

using HANDLE = void*;
struct dzn_device_memory { HANDLE export_handle; };
#define VK_FROM_HANDLE(type, name, handle) type* name = reinterpret_cast<type*>(handle)
static VkResult vk_error(VkDevice, VkResult value) { return value; }
#include "dozen_memory_fd_under_test.inc"

static void setup(bool value) { if (!value) { std::perror("DOZEN_FD_CONTRACT_SETUP_FAIL"); std::abort(); } }
static int newFile() {
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "dozen-memory-fd-contract", MFD_CLOEXEC));
    const std::uint64_t bytes = UINT64_C(0x123456789abcdef0);
    setup(fd >= 0 && ftruncate(fd, sizeof bytes) == 0 && pwrite(fd, &bytes, sizeof bytes, 0) == sizeof bytes);
    return fd;
}
static bool payload(int fd) {
    std::uint64_t value = 0;
    return fd >= 0 && pread(fd, &value, sizeof value, 0) == sizeof value && value == UINT64_C(0x123456789abcdef0);
}
static bool same(int a, int b) {
    struct stat first{}, second{};
    return a >= 0 && b >= 0 && !fstat(a, &first) && !fstat(b, &second) &&
        first.st_dev == second.st_dev && first.st_ino == second.st_ino && first.st_size == second.st_size;
}
static void closeUnique(std::initializer_list<int> values) {
    for (const int fd : std::set<int>(values)) if (fd >= 0) close(fd);
}
static dzn_device_memory memory(std::intptr_t fd) { return {reinterpret_cast<void*>(fd)}; }
static VkResult get(dzn_device_memory& owned, int& result,
                    VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = reinterpret_cast<VkDeviceMemory>(&owned); info.handleType = type;
    return dzn_GetMemoryFdKHR(VK_NULL_HANDLE, &info, &result);
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--exec-closed")) {
        errno = 0;
        return fcntl(std::atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 19;
    }
    setup(argc == 1);
    unsigned cases = 0, failed = 0;
    const auto check = [&](const char* name, bool passed) {
        ++cases; if (!passed) ++failed;
        std::printf("DOZEN_FD_CONTRACT case=%s passed=%s gpu=false\n", name, passed ? "true" : "false");
    };
    {
        auto owned = memory(-1); int fd = -77;
        const auto result = get(owned, fd);
        check("invalid-sentinel", result != VK_SUCCESS && fd == -77 && owned.export_handle == reinterpret_cast<void*>(-1));
    }
    {
        const int source = newFile(); setup(fcntl(source, F_SETFD, 0) == 0);
        auto owned = memory(source); int first = -77, second = -77;
        const auto a = get(owned, first), b = get(owned, second);
        const bool valid = a == VK_SUCCESS && b == VK_SUCCESS && first >= 0 && second >= 0 &&
            first != second && first != source && second != source && same(source, first) && same(source, second) &&
            (fcntl(first, F_GETFD) & FD_CLOEXEC) && (fcntl(second, F_GETFD) & FD_CLOEXEC) &&
            owned.export_handle == reinterpret_cast<void*>(static_cast<intptr_t>(source));
        if (first != source) closeUnique({first});
        if (second != source && second != first) closeUnique({second});
        check("repeated-fresh-cloexec", valid && payload(source));
        close(source);
    }
    {
        const int saved = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3); setup(saved >= 0);
        const int source = newFile(); setup(dup2(source, STDIN_FILENO) == STDIN_FILENO); close(source);
        auto owned = memory(0); int exported = -77;
        const auto result = get(owned, exported);
        check("descriptor-zero", result == VK_SUCCESS && exported > 0 && payload(exported) && same(0, exported) &&
            owned.export_handle == nullptr && (fcntl(exported, F_GETFD) & FD_CLOEXEC));
        closeUnique({exported, 0}); setup(dup2(saved, STDIN_FILENO) == STDIN_FILENO); close(saved);
    }
    {
        const int source = newFile(); auto owned = memory(source); int exported = -77;
        const auto result = get(owned, exported); close(source);
        check("export-survives-owner-close", result == VK_SUCCESS && exported != source && payload(exported));
        if (exported != source) closeUnique({exported});
    }
    {
        const int source = newFile(); auto owned = memory(source); int exported = -77;
        struct rlimit previous{}; setup(getrlimit(RLIMIT_NOFILE, &previous) == 0);
        auto bounded = previous; bounded.rlim_cur = std::min<rlim_t>(previous.rlim_cur, 64);
        setup(bounded.rlim_cur > 8 && setrlimit(RLIMIT_NOFILE, &bounded) == 0);
        std::vector<int> occupied;
        for (;;) {
            const int fd = fcntl(source, F_DUPFD_CLOEXEC, 3);
            if (fd < 0) { setup(errno == EMFILE); break; }
            occupied.push_back(fd);
        }
        const auto result = get(owned, exported);
        const bool valid = result == VK_ERROR_TOO_MANY_OBJECTS && exported == -77 &&
            owned.export_handle == reinterpret_cast<void*>(static_cast<intptr_t>(source)) && payload(source);
        for (const int fd : occupied) close(fd);
        setup(setrlimit(RLIMIT_NOFILE, &previous) == 0);
        check("descriptor-exhaustion-rollback", valid);
        closeUnique({source, exported});
    }
    {
        const int source = newFile(); auto owned = memory(source); int exported = -77;
        const auto result = get(owned, exported, static_cast<VkExternalMemoryHandleTypeFlagBits>(0x7fffffff));
        check("unknown-type-rollback", result != VK_SUCCESS && exported == -77 && payload(source) &&
            owned.export_handle == reinterpret_cast<void*>(static_cast<intptr_t>(source)));
        closeUnique({source, exported});
    }
    {
        auto owned = memory(static_cast<std::intptr_t>(INT_MAX) + 1); int exported = -77;
        check("descriptor-range", get(owned, exported) != VK_SUCCESS && exported == -77);
    }
    {
        const int source = newFile(); auto owned = memory(source); int exported = -77;
        const auto result = get(owned, exported);
        const auto child = fork(); setup(child >= 0);
        if (!child) {
            char number[32]; std::snprintf(number, sizeof number, "%d", exported);
            execl(argv[0], argv[0], "--exec-closed", number, static_cast<char*>(nullptr)); _exit(127);
        }
        int status = 0; setup(waitpid(child, &status, 0) == child);
        check("close-on-exec", result == VK_SUCCESS && exported != source && exported >= 0 &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0 && payload(source));
        closeUnique({source, exported});
    }
    std::printf("DOZEN_FD_CONTRACT_COMPLETE cases=%u failed=%u gpu=false\n", cases, failed);
    return failed ? 1 : 0;
}
