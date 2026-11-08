// SPDX-License-Identifier: MIT
/*
 * LD_PRELOAD spy for /dev/dxg: logs every dxgkrnl ioctl a process makes
 * (number, result, and for adapter queries the query type and size), to
 * compare the same user-mode stack under WSL2 and under virtio-dxg.
 *
 *   gcc -O2 -shared -fPIC -o dxgspy.so dxgspy.c -ldl
 *   LD_PRELOAD=./dxgspy.so app 2>spy.log
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int (*real_ioctl)(int, unsigned long, ...);

struct qai {
	uint32_t adapter;
	uint32_t type;
	uint64_t data;
	uint32_t size;
};

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret, err;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	ret = real_ioctl(fd, request, arg);
	err = errno;
	if (((request >> 8) & 0xff) == 0x47) {
		unsigned nr = request & 0xff;

		if (nr == 0x09 && arg) {
			const struct qai *q = arg;
			uint32_t first = 0;

			if (q->data && q->size >= 4)
				memcpy(&first, (void *)(uintptr_t)q->data, 4);
			fprintf(stderr, "[dxg] query type %u size %u -> %d "
				"(errno %d) first word 0x%x", q->type, q->size,
				ret, ret < 0 ? err : 0, first);
			/* UMD and OpenGL file names: version, then UTF-16. */
			if ((q->type == 1 || q->type == 2) && ret == 0 &&
			    q->size > 8) {
				const uint16_t *s = (const void *)
					((uintptr_t)q->data + 4);
				unsigned i;

				fputs(" name '", stderr);
				for (i = 0; i < (q->size - 4) / 2 && s[i]; i++)
					fputc(s[i] < 128 ? s[i] : '?', stderr);
				fputc('\'', stderr);
			}
			fputc('\n', stderr);
		} else {
			fprintf(stderr, "[dxg] ioctl 0x%02x -> %d (errno %d)\n",
				nr, ret, ret < 0 ? err : 0);
		}
	}
	errno = err;
	return ret;
}
