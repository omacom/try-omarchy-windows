/* SPDX-License-Identifier: MIT */
/* Resolve only the named graphics channel; virtio indices depend on all devices. */
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct wddm_graphics_port {
    char node[48];
    dev_t device;
};

static int wddm_port_decimal(const char** cursor, unsigned limit, unsigned* value) {
    const char* p = *cursor;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return -1;
    unsigned result = 0;
    do {
        const unsigned digit = (unsigned)(*p - '0');
        if (result > limit / 10 || (result == limit / 10 && digit > limit % 10)) return -1;
        result = result * 10 + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = result;
    return 0;
}

static int wddm_port_node_name(const char* name) {
    if (!name || strncmp(name, "vport", 5)) return 0;
    const char* p = name + 5;
    unsigned controller, port;
    if (wddm_port_decimal(&p, UINT32_MAX, &controller) || *p++ != 'p' ||
        wddm_port_decimal(&p, UINT32_MAX, &port) || *p) return 0;
    return 1;
}

static int wddm_port_device(const char* text, dev_t* device) {
    if (!text || !device) return -1;
    const char* p = text;
    unsigned major_number, minor_number;
    if (wddm_port_decimal(&p, 4095, &major_number) || !major_number || *p++ != ':' ||
        wddm_port_decimal(&p, 1048575, &minor_number) || strcmp(p, "\n")) return -1;
    *device = makedev(major_number, minor_number);
    return 0;
}

static int wddm_port_attribute(const char* root, const char* node, const char* attribute,
        char* output, size_t capacity) {
    char path[512];
    const int length = snprintf(path, sizeof path, "%s/%s/%s", root, node, attribute);
    if (length <= 0 || (size_t)length >= sizeof path || capacity < 2) { errno = EINVAL; return -1; }
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat metadata;
    int error = fstat(fd, &metadata) ? errno : (!S_ISREG(metadata.st_mode) ? EINVAL : 0);
    size_t used = 0;
    while (!error && used < capacity - 1) {
        const ssize_t count = read(fd, output + used, capacity - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        if (memchr(output + used, 0, (size_t)count)) { error = EINVAL; break; }
        used += (size_t)count;
    }
    if (!error && (!used || used == capacity - 1)) error = EINVAL;
    output[used] = 0;
    if (close(fd) && !error) error = errno;
    if (error) { errno = error; return -1; }
    return 0;
}

static int wddm_graphics_port_discover(const char* sysfs, struct wddm_graphics_port* result) {
    if (!sysfs || !result) { errno = EINVAL; return -1; }
    memset(result, 0, sizeof *result);
    DIR* directory = opendir(sysfs);
    if (!directory) return -1;
    struct wddm_graphics_port candidate = {{0}, 0};
    unsigned matches = 0, entries = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        const struct dirent* entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (++entries > 256) { error = E2BIG; break; }
        if (!wddm_port_node_name(entry->d_name)) continue;
        char name[128], device[48];
        if (wddm_port_attribute(sysfs, entry->d_name, "name", name, sizeof name)) {
            error = errno; break;
        }
        if (strcmp(name, "org.7wdev.wddm\n")) continue;
        if (++matches != 1) { error = EEXIST; break; }
        if (wddm_port_attribute(sysfs, entry->d_name, "dev", device, sizeof device)) {
            error = errno; break;
        }
        if (wddm_port_device(device, &candidate.device)) { error = EINVAL; break; }
        const size_t length = strlen(entry->d_name);
        if (length >= sizeof candidate.node) { error = EINVAL; break; }
        memcpy(candidate.node, entry->d_name, length + 1);
    }
    if (closedir(directory) && !error) error = errno;
    if (!error && matches != 1) error = ENODEV;
    if (error) { errno = error; return -1; }
    *result = candidate;
    return 0;
}

static int wddm_graphics_port_resolve(const char* sysfs, const char* devices, char* path, size_t capacity) {
    if (!devices || !path || !capacity) { errno = EINVAL; return -1; }
    path[0] = 0;
    struct wddm_graphics_port identity;
    if (wddm_graphics_port_discover(sysfs, &identity)) return -1;
    char candidate[512];
    const int size = snprintf(candidate, sizeof candidate, "%s/%s", devices, identity.node);
    if (size <= 0 || (size_t)size >= sizeof candidate || (size_t)size >= capacity) { errno = ENAMETOOLONG; return -1; }
    struct stat metadata;
    if (lstat(candidate, &metadata)) return -1;
    if (!S_ISCHR(metadata.st_mode) || metadata.st_rdev != identity.device) { errno = ENODEV; return -1; }
    memcpy(path, candidate, (size_t)size + 1);
    return 0;
}
