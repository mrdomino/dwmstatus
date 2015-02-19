#define _BSD_SOURCE
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <bsd/string.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <X11/Xlib.h>

struct TzStatus {
	char c;
	char *v;
};


static const struct TzStatus tzs[] = {
	{ .c = 'U', .v = "UTC" },
	{ .c = 'P', .v = "US/Pacific" },
};
static const char *tzmain = "Canada/Eastern";
static const char *batbase = "/sys/class/power_supply/BAT0";
static const char *ifnames[] = { "wire0", "radi0" };


static Display *dpy;

static pthread_mutex_t g_mtx;


char *
smprintf(char *fmt, ...)
{
	va_list fmtargs;
	char *ret;
	int len;

	va_start(fmtargs, fmt);
	len = vsnprintf(NULL, 0, fmt, fmtargs);
	va_end(fmtargs);

	ret = malloc(++len);
	if (ret == NULL) {
		perror("malloc");
		exit(1);
	}

	va_start(fmtargs, fmt);
	vsnprintf(ret, len, fmt, fmtargs);
	va_end(fmtargs);

	return ret;
}

void
settz(const char *tzname)
{
	setenv("TZ", tzname, 1);
}

char *
mktimes(const char *fmt, const char *tzname)
{
	char buf[129];
	time_t tim;
	struct tm *timtm;

	memset(buf, 0, sizeof(buf));
	settz(tzname);
	tim = time(NULL);
	timtm = localtime(&tim);
	if (timtm == NULL) {
		perror("localtime");
		exit(1);
	}

	if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
		fprintf(stderr, "strftime == 0\n");
		exit(1);
	}

	return smprintf("%s", buf);
}

void
setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

char *
loadavg(void)
{
	double avgs[3];

	if (getloadavg(avgs, 3) < 0) {
		perror("getloadavg");
		exit(1);
	}

	return smprintf("%.2f %.2f %.2f", avgs[0], avgs[1], avgs[2]);
}

char *
ipaddr(void)
{
	int fd, r, i;
	const char *ifname;
	struct ifreq ifr;

	ifr.ifr_addr.sa_family = AF_INET;

	if (-1 == (fd = socket(AF_INET, SOCK_DGRAM, 0))) {
		perror("socket");
		return smprintf("-");
	}
	for (i = 0; i < sizeof ifnames / sizeof *ifnames; i++) {
		ifname = ifnames[i];
		strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
		r = ioctl(fd, SIOCGIFADDR, &ifr);
		if (r == 0) {
			break;
		}
	}
	close(fd);
	if (r == -1) {
		return smprintf("-");
	} else {
		return smprintf("%s:%s", ifname, inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr));
	}
}

char *
readfile(const char *base, const char *file)
{
	char buf[2048];
	int fd;
	int r;

	r = snprintf(buf, 2048, "%s/%s", base, file);
	if (r >= 2048) {
		return 0;
	}
	if (-1 == (fd = open(buf, O_RDONLY))) {
		return 0;
	}
	if (-1 == (r = read(fd, buf, 2047))) {
		perror("read");
		r = close(fd);
		assert(r == 0);
		return 0;
	}
	assert(r < 2048);
	buf[r] = 0;

	r = close(fd);
	assert(r == 0);

	return strdup(buf);
}

char *
getbattery(const char *base)
{
	char *co;
	int descap, remcap;

	descap = -1;
	remcap = -1;

	co = readfile(base, "present");
	if (co == NULL || co[0] != '1') {
		free(co);
		return smprintf("not present");
	}
	free(co);

	co = readfile(base, "charge_full_design");
	if (co == NULL) {
		co = readfile(base, "energy_full_design");
		if (co == NULL) {
			return smprintf("");
		}
	}
	sscanf(co, "%d", &descap);
	free(co);

	co = readfile(base, "charge_now");
	if (co == NULL) {
		co = readfile(base, "energy_now");
		if (co == NULL) {
			return smprintf("");
		}
	}
	sscanf(co, "%d", &remcap);
	free(co);

	if (remcap < 0 || descap < 0) {
		return smprintf("invalid");
	}
	return smprintf("%.0f", (float)remcap / descap * 100.0);
}

void
update_status(void)
{
	char *avgs;
	char *bat;
	char *addr;
	char *status;
	char *newstatus;
	char *tm;
	int i;

	if (pthread_mutex_trylock(&g_mtx) != 0) {
		return;
	}

	avgs = loadavg();
	bat = getbattery(batbase);
	addr = ipaddr();
	status = smprintf("%s B:%s L:%s", addr, bat, avgs);
	for (i = 0; i < sizeof tzs / sizeof *tzs; i++) {
		tm = mktimes("%H:%M", tzs[i].v);
		newstatus = smprintf("%s %c:%s", status, tzs[i].c, tm);
		free(status);
		free(tm);
		status = newstatus;
	}
	tm = mktimes("%W %a %d %b %H:%M %Z %Y", tzmain);
	newstatus = smprintf("%s  %s", status, tm);
	free(status);
	free(tm);
	status = newstatus;

	setstatus(status);

	pthread_mutex_unlock(&g_mtx);
	free(avgs);
	free(bat);
	free(addr);
	free(status);
}

void
sighup(int sig)
{
	int save_errno = errno;

	fprintf(stderr, "dwmstatus: got SIGHUP.\n");
	update_status();
	errno = save_errno;
}

int
main(void)
{
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwmstatus: cannot open display.\n");
		return 1;
	}
	if (pthread_mutex_init(&g_mtx, NULL) != 0) {
		XCloseDisplay(dpy);
		return 2;
	}

	signal(SIGHUP, sighup);

	for (;;sleep(90)) {
		update_status();
	}

	signal(SIGHUP, SIG_DFL);

	XCloseDisplay(dpy);
	pthread_mutex_destroy(&g_mtx);

	return 0;
}

