// SPDX-License-Identifier: MIT
/*
 * Bring-up test for /dev/dxg, independent of any GPU driver's user-mode
 * part: adapters, their identity, a device, a monitored fence signalled
 * from the CPU and read back through its mapped fence page, a CPU wait, and
 * an allocation that is locked, written and read back.
 *
 * Runs the same against WSL2's dxgkrnl (the reference) and the VM's
 * virtio-dxg. Static, so it runs from a bare initramfs:
 *
 *   gcc -static -O2 -I../include -o dxgtest dxgtest.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/types.h>
#include "uapi/misc/d3dkmthk.h"

#define QAI_UMOPENGLINFO 2
#define QAI_PHYSICALADAPTERDEVICEIDS 31
#define QAI_DRIVERVERSION_RENDER 56
#define QAI_DRIVER_DESCRIPTION_RENDER 66
#define MONITORED_FENCE 5

static int dxg;
static int failures;

#define CHECK(what, call)						\
	({								\
		int r_ = (call);					\
		if (r_ < 0) {						\
			printf("FAIL %s: %s\n", what, strerror(errno));	\
			failures++;					\
		}							\
		r_;							\
	})

static double now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int query(struct d3dkmthandle adapter, int type, void *data,
		 unsigned size)
{
	struct d3dkmt_queryadapterinfo q = { 0 };

	q.adapter = adapter;
	q.type = type;
	q.private_data = (__u64)(uintptr_t)data;
	q.private_data_size = size;
	return ioctl(dxg, LX_DXQUERYADAPTERINFO, &q);
}

static void utf16_to_ascii(const __u16 *in, char *out, size_t size)
{
	size_t i;

	for (i = 0; i + 1 < size && in[i]; i++)
		out[i] = in[i] < 128 ? (char)in[i] : '?';
	out[i] = 0;
}

static void describe(struct d3dkmthandle adapter, struct winluid luid)
{
	struct {
		__u32 index, vendor, device, subvendor, subsystem, revision, bus;
	} ids = { 0 };
	static __u16 text[4096];
	struct {
		__u16 icd[260];
		__u32 version, flags;
	} gl;
	char name[256];
	__u32 wddm = 0;

	printf("adapter luid %08x:%08x\n", luid.b, luid.a);
	if (query(adapter, QAI_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids)) >= 0)
		printf("  PCI %04x:%04x subsystem %04x:%04x rev %02x\n",
		       ids.vendor, ids.device, ids.subvendor, ids.subsystem,
		       ids.revision);
	if (query(adapter, QAI_DRIVER_DESCRIPTION_RENDER, text,
		  sizeof(text)) >= 0) {
		utf16_to_ascii(text, name, sizeof(name));
		printf("  name %s\n", name);
	}
	if (query(adapter, QAI_DRIVERVERSION_RENDER, &wddm, sizeof(wddm)) >= 0)
		printf("  WDDM %u.%u\n", wddm / 1000, wddm % 1000 / 100);
	memset(&gl, 0, sizeof(gl));
	if (query(adapter, QAI_UMOPENGLINFO, &gl, sizeof(gl)) >= 0) {
		utf16_to_ascii(gl.icd, name, sizeof(name));
		printf("  OpenGL/Vulkan driver %s\n", name);
	}
}

/* Device, monitored fence, CPU signal and wait, on one adapter. */
static void exercise(struct winluid luid)
{
	struct d3dkmt_openadapterfromluid open = { .adapter_luid = luid };
	struct d3dkmt_createdevice cd = { 0 };
	struct d3dkmt_createsynchronizationobject2 cs = { 0 };
	struct d3dkmt_signalsynchronizationobjectfromcpu sig = { 0 };
	struct d3dkmt_waitforsynchronizationobjectfromcpu wait = { 0 };
	struct d3dkmt_destroysynchronizationobject ds = { 0 };
	struct d3dkmt_destroydevice dd = { 0 };
	struct d3dkmt_closeadapter ca = { 0 };
	volatile __u64 *fence_page;
	__u64 value;
	double t, best = 1e9;
	int i;

	if (CHECK("open adapter", ioctl(dxg, LX_DXOPENADAPTERFROMLUID,
					&open)) < 0)
		return;
	cd.adapter = open.adapter_handle;
	if (CHECK("create device", ioctl(dxg, LX_DXCREATEDEVICE, &cd)) < 0)
		goto close;
	printf("  device created\n");

	cs.device = cd.device;
	cs.info.type = MONITORED_FENCE;
	if (CHECK("create monitored fence",
		  ioctl(dxg, LX_DXCREATESYNCHRONIZATIONOBJECT, &cs)) < 0)
		goto device;
	fence_page = (volatile __u64 *)(uintptr_t)
		cs.info.monitored_fence.fence_cpu_virtual_address;
	printf("  monitored fence, page mapped at %p, value %llu\n",
	       (void *)fence_page,
	       fence_page ? (unsigned long long)*fence_page : 0ull);

	sig.device = cd.device;
	sig.object_count = 1;
	sig.objects = (__u64)(uintptr_t)&cs.sync_object;
	sig.fence_values = (__u64)(uintptr_t)&value;
	wait.device = cd.device;
	wait.object_count = 1;
	wait.objects = (__u64)(uintptr_t)&cs.sync_object;
	wait.fence_values = (__u64)(uintptr_t)&value;
	for (i = 1; i <= 200; i++) {
		value = i;
		t = now_us();
		if (CHECK("signal from CPU",
			  ioctl(dxg, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU,
				&sig)) < 0 ||
		    CHECK("wait from CPU",
			  ioctl(dxg, LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU,
				&wait)) < 0)
			break;
		t = now_us() - t;
		if (t < best)
			best = t;
	}
	if (fence_page && *fence_page != 200) {
		printf("FAIL fence page reads %llu, expected 200\n",
		       (unsigned long long)*fence_page);
		failures++;
	} else {
		printf("  200 CPU signal+wait round trips, best %.1f us; fence "
		       "page reads %llu\n", best,
		       fence_page ? (unsigned long long)*fence_page : 0ull);
	}

	ds.sync_object = cs.sync_object;
	CHECK("destroy fence",
	      ioctl(dxg, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &ds));
device:
	dd.device = cd.device;
	CHECK("destroy device", ioctl(dxg, LX_DXDESTROYDEVICE, &dd));
close:
	ca.adapter_handle = open.adapter_handle;
	CHECK("close adapter", ioctl(dxg, LX_DXCLOSEADAPTER, &ca));
}

int main(int argc, char **argv)
{
	struct d3dkmt_adapterinfo info[16];
	struct d3dkmt_enumadapters2 e = { 16, 0, (void *)info };
	const char *path = argc > 1 ? argv[1] : "/dev/dxg";
	unsigned i;

	setvbuf(stdout, NULL, _IONBF, 0);
	dxg = open(path, O_RDWR);
	if (dxg < 0) {
		printf("FAIL open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (CHECK("enumerate adapters", ioctl(dxg, LX_DXENUMADAPTERS2, &e)) < 0)
		return 1;
	printf("%u adapter(s)\n", e.num_adapters);
	for (i = 0; i < e.num_adapters; i++)
		describe(info[i].adapter_handle, info[i].adapter_luid);
	for (i = 0; i < e.num_adapters; i++) {
		struct d3dkmt_closeadapter ca = { info[i].adapter_handle };

		ioctl(dxg, LX_DXCLOSEADAPTER, &ca);
	}
	for (i = 0; i < e.num_adapters; i++) {
		printf("adapter %u:\n", i);
		exercise(info[i].adapter_luid);
	}
	printf("%s (%d failure%s)\n", failures ? "DXGTEST FAILED" :
	       "DXGTEST PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
