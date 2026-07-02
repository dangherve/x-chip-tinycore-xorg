/*
 * x-chip-touch2mouse: PocketCHIP resistive touch panel -> virtual mouse.
 *
 * Mouse-only apps (PICO-8 in particular) ignore the touch events the
 * mainline sun4i-ts driver emits. While this bridge runs, the touchscreen
 * is grabbed (EVIOCGRAB) so nothing else sees it, and its movements are
 * re-emitted through /dev/uinput as a plain relative mouse with BTN_LEFT,
 * which every app understands. Touch-and-drag moves the cursor like a
 * trackpad with the button held; the idea comes from Lana-chan's
 * touch2mouse for the PocketCHIP.
 *
 * Environment:
 *   X_CHIP_TOUCH_DEVICE        kernel device name to bridge (1c25000.rtp)
 *   X_CHIP_SCREEN_WIDTH/HEIGHT cursor speed reference, default 480x272
 *   X_CHIP_TOUCH2MOUSE_SENS    sensitivity multiplier, default 1.0;
 *                              negative values flip the direction
 *   X_CHIP_TOUCH2MOUSE_PIDFILE default /tmp/x-chip-touch2mouse.pid
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#define DEFAULT_DEVICE "1c25000.rtp"
#define DEFAULT_RANGE 65535
#define DEFAULT_PIDFILE "/tmp/x-chip-touch2mouse.pid"

static volatile sig_atomic_t running = 1;

static void stop_running(int sig)
{
	(void)sig;
	running = 0;
}

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);

	return (value && *value) ? value : fallback;
}

static int open_touchscreen(const char *wanted, char *path, size_t path_len)
{
	char name[256];
	int i, fd;

	for (i = 0; i < 64; i++) {
		snprintf(path, path_len, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		    strstr(name, wanted))
			return fd;
		close(fd);
	}
	return -1;
}

static int axis_range(int fd, int axis)
{
	struct input_absinfo info;

	if (ioctl(fd, EVIOCGABS(axis), &info) < 0 ||
	    info.maximum <= info.minimum)
		return DEFAULT_RANGE;
	return info.maximum - info.minimum;
}

static int emit(int ufd, int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(ufd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int create_mouse(void)
{
	struct uinput_setup setup;
	int ufd;

	ufd = open("/dev/uinput", O_WRONLY);
	if (ufd < 0) {
		perror("open /dev/uinput");
		if (errno == ENOENT)
			fprintf(stderr, "the kernel has no uinput support; "
				"update the system (sudo x-chip-update)\n");
		return -1;
	}
	if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
	    ioctl(ufd, UI_SET_EVBIT, EV_REL) < 0 ||
	    ioctl(ufd, UI_SET_RELBIT, REL_X) < 0 ||
	    ioctl(ufd, UI_SET_RELBIT, REL_Y) < 0) {
		perror("uinput setup ioctl");
		close(ufd);
		return -1;
	}
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1f3a;
	setup.id.product = 0x0001;
	snprintf(setup.name, sizeof(setup.name), "x-chip-touch2mouse");
	if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl(ufd, UI_DEV_CREATE) < 0) {
		perror("uinput device create");
		close(ufd);
		return -1;
	}
	/* Give udev and the X server time to pick up the new mouse. */
	sleep(1);
	return ufd;
}

static void write_pidfile(const char *pidfile)
{
	FILE *f = fopen(pidfile, "w");

	if (!f)
		return;
	fprintf(f, "%d\n", (int)getpid());
	fclose(f);
}

int main(void)
{
	const char *wanted = env_or("X_CHIP_TOUCH_DEVICE", DEFAULT_DEVICE);
	const char *pidfile = env_or("X_CHIP_TOUCH2MOUSE_PIDFILE",
				     DEFAULT_PIDFILE);
	double sens = atof(env_or("X_CHIP_TOUCH2MOUSE_SENS", "1.0"));
	int width = atoi(env_or("X_CHIP_SCREEN_WIDTH", "480"));
	int height = atoi(env_or("X_CHIP_SCREEN_HEIGHT", "272"));
	char path[64];
	struct sigaction sa;
	struct input_event ev;
	double scale_x, scale_y, acc_x = 0, acc_y = 0;
	int tfd, ufd, touching = 0, have_last = 0;
	int cur_x = 0, cur_y = 0, last_x = 0, last_y = 0;
	ssize_t n;

	if (sens == 0)
		sens = 1.0;
	if (width < 1)
		width = 480;
	if (height < 1)
		height = 272;

	tfd = open_touchscreen(wanted, path, sizeof(path));
	if (tfd < 0) {
		fprintf(stderr, "no input device matching '%s' under "
			"/dev/input (this tool needs root)\n", wanted);
		return 1;
	}

	/* The PocketCHIP rtp raw axes decrease left->right and top->bottom,
	 * so (last - cur) already points the natural way. */
	scale_x = (double)width * sens / axis_range(tfd, ABS_X);
	scale_y = (double)height * sens / axis_range(tfd, ABS_Y);

	ufd = create_mouse();
	if (ufd < 0) {
		close(tfd);
		return 1;
	}

	if (ioctl(tfd, EVIOCGRAB, (void *)1) < 0)
		perror("EVIOCGRAB (continuing without exclusive grab)");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = stop_running;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	write_pidfile(pidfile);
	printf("bridging %s (%s) as a relative mouse\n", path, wanted);

	while (running) {
		n = read(tfd, &ev, sizeof(ev));
		if (n != (ssize_t)sizeof(ev)) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			touching = ev.value;
			if (ev.value) {
				have_last = 0;
				acc_x = acc_y = 0;
			}
			emit(ufd, EV_KEY, BTN_LEFT, ev.value);
			emit(ufd, EV_SYN, SYN_REPORT, 0);
		} else if (ev.type == EV_ABS && ev.code == ABS_X) {
			cur_x = ev.value;
		} else if (ev.type == EV_ABS && ev.code == ABS_Y) {
			cur_y = ev.value;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT &&
			   touching) {
			if (have_last) {
				int dx, dy;

				acc_x += (last_x - cur_x) * scale_x;
				acc_y += (last_y - cur_y) * scale_y;
				dx = (int)acc_x;
				dy = (int)acc_y;
				if (dx || dy) {
					acc_x -= dx;
					acc_y -= dy;
					emit(ufd, EV_REL, REL_X, dx);
					emit(ufd, EV_REL, REL_Y, dy);
					emit(ufd, EV_SYN, SYN_REPORT, 0);
				}
			}
			last_x = cur_x;
			last_y = cur_y;
			have_last = 1;
		}
	}

	ioctl(tfd, EVIOCGRAB, (void *)0);
	ioctl(ufd, UI_DEV_DESTROY);
	close(ufd);
	close(tfd);
	unlink(pidfile);
	return 0;
}
