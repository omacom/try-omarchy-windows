/* wheelgen [HZ=240] [SECS=2] [UNITS=30]: a virtual mouse (uinput) that turns
 * its high-resolution wheel at a steady rate: UNITS (120 = one notch) every
 * 1/HZ s, SECS down then SECS up, timed with absolute clock_nanosleep. Input
 * as a real high-resolution wheel delivers it, without the QMP/TCP path's
 * bursts. Needs write access to /dev/uinput.
 * Build: gcc -O2 wheelgen.c -o wheelgen */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void emit(int type, int code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
}

static void add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec++;
	}
}

int main(int argc, char **argv)
{
	int hz = argc > 1 ? atoi(argv[1]) : 240;
	double secs = argc > 2 ? atof(argv[2]) : 2;
	int units = argc > 3 ? atoi(argv[3]) : 30;
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
	struct uinput_setup us;
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1d6b;
	us.id.product = 0x0f17;
	snprintf(us.name, UINPUT_MAX_NAME_SIZE, "wheelgen high-resolution wheel");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);
	sleep(1);   /* libinput and the compositor pick the device up */

	long period = 1000000000L / hz;
	int n = (int)(secs * hz), acc = 0;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	for (int dir = -1; dir <= 1; dir += 2) {
		for (int i = 0; i < n; i++) {
			add_ns(&t, period);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
			emit(EV_REL, REL_WHEEL_HI_RES, dir * units);
			acc += units;
			if (acc >= 120) {   /* as kernel HID drivers do */
				acc -= 120;
				emit(EV_REL, REL_WHEEL, dir);
			}
			emit(EV_SYN, SYN_REPORT, 0);
		}
	}
	usleep(200000);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
