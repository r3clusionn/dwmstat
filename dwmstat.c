/* dwmstat: event-driven dwm status text (root window name).
 *
 * Sleeps in poll() and only wakes for:
 *   - the minute boundary (clock, battery time estimate, volume resync)
 *   - kernel uevents for power_supply / rfkill / bluetooth (AC plug, battery
 *     status, radio switches, Bluetooth connections)
 *   - rtnetlink link changes (Wi-Fi connect/disconnect)
 *   - SIGUSR1, sent by bl/vol/audio/bt-toggle/powerprofile after they change something
 * It never starts PipeWire or bluetoothd: it only looks at their state.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <X11/Xlib.h>

/* from bluez lib/hci.h; avoids a bluez-libs build dependency */
#define BTPROTO_HCI   1
#define HCIGETDEVINFO _IOR('H', 211, int)
#define HCI_FLAGS_OFF 16 /* offsetof(struct hci_dev_info, flags) */
#define HCI_UP        (1u << 0)

static int
readstr(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	while (r > 0 && buf[r - 1] == '\n')
		r--;
	buf[r] = '\0';
	return 0;
}

static long
readnum(const char *dir, const char *file)
{
	char path[512], buf[32];

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (readstr(path, buf, sizeof(buf)) < 0)
		return -1;
	return strtol(buf, NULL, 10);
}

static void
brightness(char *out, size_t n)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	char dir[300];
	long cur, max;

	*out = '\0';
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof(dir), "/sys/class/backlight/%s", e->d_name);
		max = readnum(dir, "max_brightness");
		cur = readnum(dir, "brightness");
		if (max > 0 && cur >= 0) {
			snprintf(out, n, "☀ %ld%%  ", (cur * 100 + max / 2) / max);
			break;
		}
	}
	closedir(d);
}

/* PipeWire is up if its socket accepts a connection (a stale socket file
 * from a killed daemon refuses). Never launches anything. */
static int
pipewire_up(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *rt = getenv("XDG_RUNTIME_DIR");
	int s, ok;

	snprintf(a.sun_path, sizeof(a.sun_path), "%s/pipewire-0", rt ? rt : "/tmp");
	if ((s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
		return 0;
	ok = connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
	close(s);
	return ok;
}

static void
volume(char *out, size_t n)
{
	struct pollfd pfd;
	sigset_t none;
	char buf[128];
	size_t len = 0;
	ssize_t r;
	float v;
	pid_t pid;
	int p[2], fd;

	if (!pipewire_up()) {
		snprintf(out, n, "♪ off");
		return;
	}
	if (pipe2(p, O_CLOEXEC) < 0 || (pid = fork()) < 0) {
		snprintf(out, n, "♪ ?");
		return;
	}
	if (pid == 0) {
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		dup2(p[1], 1);
		if ((fd = open("/dev/null", O_RDWR)) >= 0) {
			dup2(fd, 0);
			dup2(fd, 2);
		}
		execlp("wpctl", "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	pfd.fd = p[0];
	pfd.events = POLLIN;
	while (len < sizeof(buf) - 1 && poll(&pfd, 1, 1000) > 0) {
		if ((r = read(p[0], buf + len, sizeof(buf) - 1 - len)) <= 0)
			break;
		len += r;
	}
	buf[len] = '\0';
	close(p[0]);
	kill(pid, SIGKILL); /* no-op if it already exited; stops a hung wpctl */
	waitpid(pid, NULL, 0);

	if (sscanf(buf, "Volume: %f", &v) == 1)
		snprintf(out, n, "♪ %d%%%s", (int)(v * 100 + 0.5),
		         strstr(buf, "MUTED") ? " mute" : "");
	else
		snprintf(out, n, "♪ ?");
}

static void
bluetooth(char *out, size_t n)
{
	unsigned char info[128] = { 0 }; /* struct hci_dev_info, dev_id 0 */
	struct dirent *e;
	uint32_t flags = 0;
	int s, conns = 0;
	DIR *d;

	if ((s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI)) >= 0) {
		if (ioctl(s, HCIGETDEVINFO, info) == 0)
			memcpy(&flags, info + HCI_FLAGS_OFF, sizeof(flags));
		close(s);
	}
	if (!(flags & HCI_UP)) {
		snprintf(out, n, "Ƀ off");
		return;
	}
	/* each ACL link shows up as /sys/class/bluetooth/hciX:HANDLE */
	if ((d = opendir("/sys/class/bluetooth"))) {
		while ((e = readdir(d)))
			if (strchr(e->d_name, ':'))
				conns++;
		closedir(d);
	}
	if (conns)
		snprintf(out, n, "Ƀ %d", conns);
	else
		snprintf(out, n, "Ƀ on");
}

static void
wifi(char *out, size_t n)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	char path[300], state[16];
	int up = 0;

	while (d && (e = readdir(d))) {
		snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", e->d_name);
		if (e->d_name[0] == '.' || access(path, F_OK) < 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", e->d_name);
		if (readstr(path, state, sizeof(state)) == 0 && !strcmp(state, "up"))
			up = 1;
	}
	if (d)
		closedir(d);
	snprintf(out, n, "⇅ %s", up ? "on" : "off");
}

static void
battery(char *out, size_t n)
{
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	char dir[300], path[320], buf[32], status[32];
	long cap, now, rate, mins;

	*out = '\0';
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", e->d_name);
		snprintf(path, sizeof(path), "%s/type", dir);
		if (readstr(path, buf, sizeof(buf)) < 0 || strcmp(buf, "Battery"))
			continue;
		snprintf(path, sizeof(path), "%s/scope", dir); /* skip mice etc. */
		if (readstr(path, buf, sizeof(buf)) == 0 && !strcmp(buf, "Device"))
			continue;
		snprintf(path, sizeof(path), "%s/status", dir);
		if ((cap = readnum(dir, "capacity")) < 0 ||
		    readstr(path, status, sizeof(status)) < 0)
			continue;

		if (!strcmp(status, "Charging")) {
			snprintf(out, n, "  ⚡%ld%%+", cap);
		} else if (!strcmp(status, "Discharging")) {
			if ((now = readnum(dir, "energy_now")) >= 0)
				rate = readnum(dir, "power_now");
			else if ((now = readnum(dir, "charge_now")) >= 0)
				rate = readnum(dir, "current_now");
			else
				rate = -1;
			if (rate > 0) {
				mins = now * 60 / rate;
				snprintf(out, n, "  ⚡%ld%% %ld:%02ld", cap, mins / 60, mins % 60);
			} else {
				snprintf(out, n, "  ⚡%ld%%", cap);
			}
		} else {
			snprintf(out, n, "  ⚡%ld%%", cap);
		}
		break;
	}
	if (d)
		closedir(d);
}

/* The power plan from powerprofile's state file: "⚙ bal" when picked by hand,
 * "⚙ auto:bal" when it follows the charger. Nothing if there is no file. */
static void
plan(char *out, size_t n)
{
	static const char *names[][2] = {
		{ "battery", "bat" }, { "balanced", "bal" }, { "performance", "perf" }, { "gaming", "game" },
	};
	char buf[64], name[32] = "", how[16] = "";
	const char *s;
	size_t i;

	*out = '\0';
	if (readstr("/run/powerprofile", buf, sizeof(buf)) < 0 ||
	    sscanf(buf, "%31s %15s", name, how) < 1)
		return;
	s = name;
	for (i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (!strcmp(name, names[i][0]))
			s = names[i][1];
	snprintf(out, n, "⚙ %s%s  ", strcmp(how, "pinned") ? "auto:" : "", s);
}

static int
nlsock(int proto, unsigned int groups)
{
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = groups };
	int s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, proto);

	if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(s);
		s = -1;
	}
	return s;
}

/* Drain a netlink socket; return 1 if anything relevant arrived. */
static int
drain(int fd, int uevent)
{
	static const char *subsys[] = {
		"SUBSYSTEM=power_supply", "SUBSYSTEM=rfkill", "SUBSYSTEM=bluetooth",
	};
	char buf[8192];
	ssize_t r;
	size_t i;
	int hit = 0;

	while ((r = recv(fd, buf, sizeof(buf), 0)) > 0) {
		if (!uevent) {
			hit = 1;
			continue;
		}
		for (i = 0; i < sizeof(subsys) / sizeof(*subsys); i++)
			if (memmem(buf, r, subsys[i], strlen(subsys[i]) + 1))
				hit = 1;
	}
	return hit;
}

int
main(void)
{
	char pp[32], bri[32], vol[32], bt[32], net[32], bat[48], clk[64];
	char status[256], last[256] = "";
	struct signalfd_siginfo si;
	struct pollfd fds[4];
	struct timespec ts;
	sigset_t mask;
	Display *dpy;
	time_t t;
	int timeout, update;

	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("dwmstat: cannot open display\n", stderr);
		return 1;
	}
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	fds[0].fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	fds[1].fd = nlsock(NETLINK_KOBJECT_UEVENT, 1);
	fds[2].fd = nlsock(NETLINK_ROUTE, RTMGRP_LINK);
	fds[3].fd = ConnectionNumber(dpy); /* readable on X shutdown: exit with X */
	for (int i = 0; i < 4; i++)
		fds[i].events = POLLIN;

	volume(vol, sizeof(vol));
	for (;;) {
		plan(pp, sizeof(pp));
		brightness(bri, sizeof(bri));
		bluetooth(bt, sizeof(bt));
		wifi(net, sizeof(net));
		battery(bat, sizeof(bat));
		t = time(NULL);
		strftime(clk, sizeof(clk), "%a %d %b %H:%M", localtime(&t));
		snprintf(status, sizeof(status), "%s%s%s  %s  %s%s  %s ",
		         pp, bri, vol, bt, net, bat, clk);
		if (strcmp(status, last)) {
			XStoreName(dpy, DefaultRootWindow(dpy), status);
			XFlush(dpy);
			memcpy(last, status, sizeof(last));
		}

		do {
			/* sleep until just after the next minute boundary */
			clock_gettime(CLOCK_REALTIME, &ts);
			timeout = 60000 - (int)(ts.tv_sec % 60) * 1000 -
			          (int)(ts.tv_nsec / 1000000) + 20;
			update = 0;
			if (poll(fds, 4, timeout) == 0) {
				volume(vol, sizeof(vol));
				break;
			}
			if (fds[0].revents & POLLIN) {
				while (read(fds[0].fd, &si, sizeof(si)) == sizeof(si)) {
					if (si.ssi_signo != SIGUSR1)
						return 0;
					update = 1;
				}
				if (update)
					volume(vol, sizeof(vol));
			}
			if (fds[1].fd >= 0 && (fds[1].revents & POLLIN) && drain(fds[1].fd, 1))
				update = 1;
			if (fds[2].fd >= 0 && (fds[2].revents & POLLIN) && drain(fds[2].fd, 0))
				update = 1;
			if (fds[3].revents)
				while (XPending(dpy)) /* IO error handler exits if X is gone */
					XNextEvent(dpy, &(XEvent){ 0 });
		} while (!update);
	}
}
