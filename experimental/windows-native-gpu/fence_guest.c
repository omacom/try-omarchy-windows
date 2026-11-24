// SPDX-License-Identifier: MIT
// Disk-free Linux init for the host-controlled PCI fence aperture diagnostic.
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_config(int fd, uint32_t offset, void *data, size_t size) {
    return pread(fd, data, size, offset) == (ssize_t)size;
}

static int check_device(const char *name, unsigned *found, unsigned *nonzero) {
    char path[512];
    if (snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/config", name) >= (int)sizeof path) return 1;
    int config = open(path, O_RDONLY);
    uint32_t identity = 0;
    if (config < 0) return 1;
    if (!read_config(config, 0, &identity, sizeof identity)) { close(config); return 1; }
    if (identity != 0x11fe1234) { close(config); return 0; }
    ++*found;
    uint32_t magic = 0, offset = 0, before = 0, after = 0;
    uint64_t expected = 0;
    int valid = read_config(config, 0x40, &magic, sizeof magic) &&
        read_config(config, 0x44, &offset, sizeof offset) &&
        read_config(config, 0x48, &expected, sizeof expected) &&
        read_config(config, 0x50, &before, sizeof before) &&
        magic == 0x31434e46 && offset <= 4088 && offset % 8 == 0;
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/enable", name);
    int enable = open(path, O_WRONLY);
    valid = valid && enable >= 0 && write(enable, "1", 1) == 1;
    if (enable >= 0) close(enable);
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", name);
    int resource = valid ? open(path, O_RDONLY | O_SYNC) : -1;
    void *mapping = resource >= 0 ? mmap(NULL, 4096, PROT_READ, MAP_SHARED, resource, 0) : MAP_FAILED;
    if (resource >= 0) close(resource);
    valid = valid && mapping != MAP_FAILED;
    uint64_t observed = 0;
    if (valid) {
        volatile uint64_t *fence = (volatile uint64_t *)((char *)mapping + offset);
        for (unsigned i = 0; i < 10000; ++i) {
            observed = *fence;
            if (observed != expected) { valid = 0; break; }
        }
    }
    if (mapping != MAP_FAILED && munmap(mapping, 4096)) valid = 0;
    valid = read_config(config, 0x50, &after, sizeof after) && valid;
    close(config);
    const int direct = before == after;
    if (valid && expected) ++*nonzero;
    printf("FENCE_READ direct=%s match=%s value=%" PRIu64 " loads=10000 emulatedDelta=%u\n",
        direct ? "true" : "false", valid ? "true" : "false", observed, after - before);
    return !valid || !direct;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/sys", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    unsigned found = 0, nonzero = 0;
    int failed = 0;
    DIR *devices = opendir("/sys/bus/pci/devices");
    if (devices) {
        struct dirent *entry;
        while ((entry = readdir(devices))) {
            if (entry->d_name[0] != '.') failed |= check_device(entry->d_name, &found, &nonzero);
        }
        closedir(devices);
    } else failed = 1;
    failed |= found != 2 || nonzero != 1;
    printf("FENCE_GUEST_EXIT=%d devices=%u nonzero=%u\n", !!failed, found, nonzero);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
