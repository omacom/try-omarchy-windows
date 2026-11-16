// SPDX-License-Identifier: MIT
// Disk-free acceptance init. No root disk, networking, or guest installation.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/sys", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) && errno != EBUSY) perror("devtmpfs");
    mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL);
    const char *port = "/dev/vport0p1";
    for (int i = 0; i < 200 && access(port, F_OK); ++i) usleep(50000);
    int status = 1;
    if (!access(port, F_OK)) {
        pid_t child = fork();
        if (child == 0) { execl("/guest-probe", "/guest-probe", port, (char*)NULL); perror("exec probe"); _exit(1); }
        if (child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status)) status = WEXITSTATUS(status);
        else status = 1;
    } else puts("FAIL: virtio console port missing (test kernel must include virtio_console)");
    printf("BRIDGE_GUEST_EXIT=%d\n", status);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
