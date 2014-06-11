#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#include <sys/sysctl.h>
#include <sys/sensors.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <X11/Xlib.h>

struct TzStatus {
	char c;
	char *v;
};


static const struct TzStatus tzs[] = {
	{ .c = 'U', .v = "UTC" },
	{ .c = 'E', .v = "US/Eastern" },
};
static const char *tzmain = "US/Pacific";
static const char *bat = "acpibat0";
static const char *ifnames[] = { "em0", "iwn0" };


static Display *dpy;

static pthread_mutex_t g_mtx;

static int g_dev = -1;

/*
 * from acpidev.h
 */
#define BST_DISCHARGE 0x01
#define BST_CHARGE 0x02

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
	char *ifname;
	struct ifreq ifr;

	ifr.ifr_addr.sa_family = AF_INET;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	for (i = 0; i < sizeof ifnames / sizeof *ifnames; i++) {
		ifname = ifnames[i];
		strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
		r = ioctl(fd, SIOCGIFADDR, &ifr);
		if (r == -1) {
			continue;
		}
	}
	close(fd);
	if (r == -1) {
		return smprintf("-");
	} else {
		return smprintf("%s:%s", ifname, inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr));
	}
}

int
findbat(void)
{
	int mib[3];
	struct sensordev sd;
	size_t len = sizeof(sd);
	int dev;

	mib[0] = CTL_HW;
	mib[1] = HW_SENSORS;

	for (dev = 0; ; dev++) {
		mib[2] = dev;
		if (sysctl(mib, 3, &sd, &len, NULL, 0) == -1)
			perror("sysctl");
		if (strcmp(sd.xname, bat) == 0)
			break;
	}
	if (strcmp(sd.xname, bat) != 0)
		return -1;
	return dev;
}

char *
batstat(void)
{
	int mib[5];
	enum sensor_type rate_type;
	struct sensor sens;
	size_t len = sizeof(sens);
	int64_t full, rem, rate;

	if (g_dev < 0)
		g_dev = findbat();

	mib[0] = CTL_HW;
	mib[1] = HW_SENSORS;
	mib[2] = g_dev;
	mib[4] = 0;

	/* Check whether measurement is amps or watts */
	mib[3] = SENSOR_AMPHOUR;
	rate_type = SENSOR_AMPS;
	if (sysctl(mib, 5, &sens, &len, NULL, 0) == -1) {
		mib[3] = SENSOR_WATTHOUR;
		rate_type = SENSOR_WATTS;
	}

	if (sysctl(mib, 5, &sens, &len, NULL, 0) == -1)
		perror("sysctl");
	if (strcmp(sens.desc, "last full capacity") != 0)
		return smprintf("expected full, got %s", sens.desc);
	full = sens.value;

	mib[4] = 3;
	if (sysctl(mib, 5, &sens, &len, NULL, 0) == -1)
		perror("sysctl");
	if (strcmp(sens.desc, "remaining capacity") != 0)
		return smprintf("expected rem, got %s", sens.desc);
	rem = sens.value;

	mib[3] = rate_type;
	mib[4] = 0;
	if (sysctl(mib, 5, &sens, &len, NULL, 0) == -1)
		perror("sysctl");
	if (strcmp(sens.desc, "rate") != 0)
		return smprintf("expected rate, got %s", sens.desc);
	rate = sens.value;

	/* get status */
	mib[3] = SENSOR_INTEGER;
	if (sysctl(mib, 5, &sens, &len, NULL, 0) == -1) {
		return smprintf("no status");
	}

	if (rate == 0 || sens.value == 0) {
		return smprintf("%d%%", rem / (full / 100));
	} else if (sens.value & BST_DISCHARGE) {
		return smprintf("%d%%- %d:%02d", rem / (full / 100),
		                rem / rate, (rem * 60 / rate) % 60);
	} else if (sens.value & BST_CHARGE) {
		return smprintf("%d%%+ %d:%02d", rem / (full / 100),
		                (full - rem) / rate,
		                ((full - rem) * 60 / rate) % 60);
	} else {
		return smprintf("unknown status %x", sens.value);
	}
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

	avgs = loadavg();
	bat = batstat();
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
	free(avgs);
	free(bat);
	free(addr);
	free(status);
}

void
sighup(int sig)
{
	fprintf(stderr, "dwmstatus: got SIGHUP.\n");
	if (pthread_mutex_trylock(&g_mtx) == 0) {
		update_status();
		pthread_mutex_unlock(&g_mtx);
	}
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
		if (pthread_mutex_trylock(&g_mtx) == 0) {
			update_status();
			pthread_mutex_unlock(&g_mtx);
		}
	}

	XCloseDisplay(dpy);
	pthread_mutex_destroy(&g_mtx);

	return 0;
}

