// SPDX-License-Identifier: MIT
/*
 * /init of the bring-up initramfs: loads dxgkrnl_virtio.ko, waits for
 * /dev/dxg, runs /dxgtest and powers the VM off. Output goes to the
 * console (the serial port). No guest disk is attached or touched.
 *
 *   gcc -static -O2 -o init init.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void run(char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid == 0) {
		execv(argv[0], argv);
		printf("init: cannot run %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	waitpid(pid, &status, 0);
	printf("init: %s exited with %d\n", argv[0],
	       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

int main(void)
{
	char *test[] = { "/dxgtest", NULL };
	struct timespec tick = { 0, 50 * 1000 * 1000 };
	int fd, i;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("init: virtio-dxg bring-up\n");

	fd = open("/dxgkrnl_virtio.ko", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || syscall(SYS_finit_module, fd, "", 0) != 0)
		printf("init: loading dxgkrnl_virtio.ko failed: %s\n",
		       strerror(errno));
	else
		printf("init: dxgkrnl_virtio.ko loaded\n");

	for (i = 0; i < 100 && access("/dev/dxg", F_OK) != 0; i++)
		nanosleep(&tick, NULL);
	if (access("/dev/dxg", F_OK) != 0)
		printf("init: /dev/dxg did not appear\n");
	else
		run(test);

	sync();
	reboot(RB_POWER_OFF);
	return 0;
}
