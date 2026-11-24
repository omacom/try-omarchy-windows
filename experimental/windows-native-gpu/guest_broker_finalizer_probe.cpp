// SPDX-License-Identifier: MIT
// Owned Linux broker lifetime contract DSO. Never loads a GPU runtime.
#include <cstdint>
#include <unistd.h>
static int channel = -1, notify = -1, gate = -1, backing = -1;
extern "C" int broker_finalizer_probe_init(int socket, int report, int proceed, int memory) {
    if (channel >= 0 || socket < 0 || report < 0 || proceed < 0 || memory < 0) return -1;
    channel = socket; notify = report; gate = proceed; backing = memory;
    return 0;
}
__attribute__((destructor)) static void finalize() {
    if (channel < 0) return;
    ::close(channel); channel = -1;
    char command = 0;
    if (::write(notify, "C", 1) != 1 || ::read(gate, &command, 1) != 1 || command != 'F') _exit(81);
    std::uint64_t value = 0;
    if (pread(backing, &value, sizeof(value), 0) != sizeof(value) || value != UINT64_C(0x793251624435feda)) _exit(82);
    if (::write(notify, "D", 1) != 1 || ::read(gate, &command, 1) != 1 || command != 'E') _exit(83);
}
