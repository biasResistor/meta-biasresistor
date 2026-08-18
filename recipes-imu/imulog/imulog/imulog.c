/*
 * imulog - poll an InvenSense MPU6050 through the IIO sysfs interface and
 * write one CSV line per sample.
 *
 * The board this targets has no interrupt line wired to the sensor, so the
 * IIO buffer/trigger path is unavailable and the only way to get data out is
 * to read the per-channel sysfs attributes. Values follow the IIO ABI:
 *
 *     processed = (raw + offset) * scale
 *
 * with accelerometer scale in m/s^2 per count, angular velocity in rad/s per
 * count, and temperature in millidegrees Celsius.
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IIO_DEVICES     "/sys/bus/iio/devices"
#define PATH_MAX_LEN    256
#define MAX_AXES        3
#define ATTR_MAX        64
#define VALUE_MAX       64
#define MAX_READ_ERRORS 10

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

/*
 * One IIO channel group: the three accelerometer axes, the three gyroscope
 * axes, or the single temperature channel. Scale and offset are shared by the
 * axes of a group, so they are read once at startup rather than per sample.
 */
struct group {
	const char *label;	/* for error messages */
	const char *prefix;	/* sysfs attribute prefix, e.g. "in_accel" */
	int naxes;		/* 3 for x/y/z, 1 for a scalar channel */
	double scale;
	double offset;
	int fd[MAX_AXES];	/* held open for the life of the process */
};

static struct group groups[] = {
	{ "accelerometer", "in_accel",   3, 1.0, 0.0, { -1, -1, -1 } },
	{ "gyroscope",     "in_anglvel", 3, 1.0, 0.0, { -1, -1, -1 } },
	{ "temperature",   "in_temp",    1, 1.0, 0.0, { -1, -1, -1 } },
};

#define NGROUPS ((int)(sizeof(groups) / sizeof(groups[0])))

static const char *const axis_name[MAX_AXES] = { "x", "y", "z" };

/*
 * Read a sysfs attribute into buf. Sysfs regenerates the value whenever it is
 * read at offset 0, so pread() on a long-lived descriptor returns a fresh
 * sample without the open/close pair every time round the loop.
 */
static int read_attr_fd(int fd, char *buf, size_t len)
{
	ssize_t n = pread(fd, buf, len - 1, 0);

	if (n < 0)
		return -1;

	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';

	return 0;
}

static int read_attr(const char *dir, const char *attr, char *buf, size_t len)
{
	char path[PATH_MAX_LEN];
	int fd, ret;

	if (snprintf(path, sizeof(path), "%s/%s", dir, attr) >= (int)sizeof(path))
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	ret = read_attr_fd(fd, buf, len);
	close(fd);

	return ret;
}

/* Read a floating point attribute, leaving *out untouched if it is absent. */
static int read_attr_double(const char *dir, const char *attr, double *out)
{
	char buf[VALUE_MAX];

	if (read_attr(dir, attr, buf, sizeof(buf)) != 0)
		return -1;

	*out = strtod(buf, NULL);

	return 0;
}

/*
 * Locate the IIO device directory whose "name" attribute matches. Device
 * numbering depends on probe order, so matching by name beats hardcoding
 * iio:device0.
 */
static int find_device(const char *want, char *out, size_t len)
{
	struct dirent *ent;
	DIR *d;
	int found = -1;

	d = opendir(IIO_DEVICES);
	if (!d) {
		fprintf(stderr, "imulog: %s: %s\n", IIO_DEVICES, strerror(errno));
		return -1;
	}

	while ((ent = readdir(d))) {
		char dir[PATH_MAX_LEN];
		char name[VALUE_MAX];

		if (strncmp(ent->d_name, "iio:device", 10) != 0)
			continue;

		if (snprintf(dir, sizeof(dir), "%s/%s", IIO_DEVICES,
			     ent->d_name) >= (int)sizeof(dir))
			continue;

		if (read_attr(dir, "name", name, sizeof(name)) != 0)
			continue;

		if (strcmp(name, want) != 0)
			continue;

		if (snprintf(out, len, "%s", dir) >= (int)len)
			continue;

		found = 0;
		break;
	}

	closedir(d);

	if (found != 0)
		fprintf(stderr, "imulog: no IIO device named '%s' under %s\n",
			want, IIO_DEVICES);

	return found;
}

/* Open the raw attributes of one group and pick up its scale and offset. */
static int open_group(struct group *g, const char *dir)
{
	char attr[ATTR_MAX];
	char path[PATH_MAX_LEN + ATTR_MAX];
	int i;

	for (i = 0; i < g->naxes; i++) {
		if (g->naxes > 1)
			snprintf(attr, sizeof(attr), "%s_%s_raw", g->prefix,
				 axis_name[i]);
		else
			snprintf(attr, sizeof(attr), "%s_raw", g->prefix);

		if (snprintf(path, sizeof(path), "%s/%s", dir, attr) >=
		    (int)sizeof(path))
			return -1;

		g->fd[i] = open(path, O_RDONLY | O_CLOEXEC);
		if (g->fd[i] < 0) {
			fprintf(stderr, "imulog: %s: %s\n", path,
				strerror(errno));
			return -1;
		}
	}

	/*
	 * Scale is normally shared by the whole group; some drivers expose it
	 * per axis instead. Offset is optional and defaults to zero.
	 */
	snprintf(attr, sizeof(attr), "%s_scale", g->prefix);
	if (read_attr_double(dir, attr, &g->scale) != 0 && g->naxes > 1) {
		snprintf(attr, sizeof(attr), "%s_x_scale", g->prefix);
		read_attr_double(dir, attr, &g->scale);
	}

	snprintf(attr, sizeof(attr), "%s_offset", g->prefix);
	read_attr_double(dir, attr, &g->offset);

	return 0;
}

static void close_groups(void)
{
	int i, j;

	for (i = 0; i < NGROUPS; i++)
		for (j = 0; j < groups[i].naxes; j++)
			if (groups[i].fd[j] >= 0)
				close(groups[i].fd[j]);
}

static double elapsed_since(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (double)(now.tv_sec - start->tv_sec) +
	       (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void add_nsec(struct timespec *t, long long nsec)
{
	t->tv_nsec += (long)(nsec % 1000000000LL);
	t->tv_sec += (time_t)(nsec / 1000000000LL);

	if (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec++;
	}
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: imulog [-d DIR] [-m NAME] [-r HZ] [-c COUNT] [-R] [-q]\n"
		"\n"
		"  -d DIR    IIO device directory (default: search by name)\n"
		"  -m NAME   IIO device name to match (default: mpu6050)\n"
		"  -r HZ     sample rate in Hz (default: 1)\n"
		"  -c COUNT  stop after COUNT samples (default: run until signalled)\n"
		"  -R        print raw counts instead of scaled values\n"
		"  -q        no header and no summary on stderr\n"
		"  -h        this help\n");
}

int main(int argc, char *argv[])
{
	const char *dev_name = "mpu6050";
	const char *dev_dir = NULL;
	char found_dir[PATH_MAX_LEN];
	double rate = 1.0;
	long long count = 0, taken = 0;
	int raw_mode = 0, quiet = 0;
	int consecutive_errors = 0;
	struct timespec start, next;
	long long period_ns;
	int opt, i, j;

	while ((opt = getopt(argc, argv, "d:m:r:c:Rqh")) != -1) {
		switch (opt) {
		case 'd':
			dev_dir = optarg;
			break;
		case 'm':
			dev_name = optarg;
			break;
		case 'r':
			rate = strtod(optarg, NULL);
			if (!(rate > 0.0)) {
				fprintf(stderr, "imulog: rate must be > 0\n");
				return 2;
			}
			break;
		case 'c':
			count = strtoll(optarg, NULL, 10);
			if (count < 0) {
				fprintf(stderr, "imulog: count must be >= 0\n");
				return 2;
			}
			break;
		case 'R':
			raw_mode = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	if (!dev_dir) {
		if (find_device(dev_name, found_dir, sizeof(found_dir)) != 0)
			return 1;
		dev_dir = found_dir;
	}

	for (i = 0; i < NGROUPS; i++) {
		if (open_group(&groups[i], dev_dir) != 0) {
			fprintf(stderr, "imulog: %s channels unavailable in %s\n",
				groups[i].label, dev_dir);
			close_groups();
			return 1;
		}
	}

	/* Line buffered so the stream stays live when piped into a log. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (!quiet) {
		fprintf(stderr, "imulog: %s (%s) at %g Hz\n", dev_dir, dev_name,
			rate);
		if (raw_mode)
			printf("# t_s,ax,ay,az,gx,gy,gz,temp (raw counts)\n");
		else
			printf("# t_s,ax_ms2,ay_ms2,az_ms2,gx_rads,gy_rads,gz_rads,temp_c\n");
	}

	period_ns = (long long)(1e9 / rate);
	clock_gettime(CLOCK_MONOTONIC, &start);
	next = start;

	while (!stop_requested && (count == 0 || taken < count)) {
		char buf[VALUE_MAX];
		double value[NGROUPS][MAX_AXES];
		double t;
		int failed = 0;

		t = elapsed_since(&start);

		for (i = 0; i < NGROUPS; i++) {
			for (j = 0; j < groups[i].naxes; j++) {
				if (read_attr_fd(groups[i].fd[j], buf,
						 sizeof(buf)) != 0) {
					fprintf(stderr,
						"imulog: reading %s: %s\n",
						groups[i].label,
						strerror(errno));
					failed = 1;
					break;
				}

				value[i][j] = strtod(buf, NULL);
				if (!raw_mode)
					value[i][j] = (value[i][j] +
						       groups[i].offset) *
						      groups[i].scale;
			}

			if (failed)
				break;
		}

		if (failed) {
			if (++consecutive_errors >= MAX_READ_ERRORS) {
				fprintf(stderr,
					"imulog: giving up after %d failed reads\n",
					consecutive_errors);
				close_groups();
				return 1;
			}
		} else {
			consecutive_errors = 0;

			/*
			 * Temperature comes out of the ABI in millidegrees
			 * Celsius; everything else is already in its unit.
			 */
			printf("%.3f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.2f\n", t,
			       value[0][0], value[0][1], value[0][2],
			       value[1][0], value[1][1], value[1][2],
			       raw_mode ? value[2][0] : value[2][0] / 1000.0);

			taken++;
		}

		if (count != 0 && taken >= count)
			break;	/* no point sleeping out the last period */

		add_nsec(&next, period_ns);

		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next,
				       NULL) == EINTR && !stop_requested)
			;
	}

	if (!quiet)
		fprintf(stderr, "imulog: %lld samples in %.1f s\n", taken,
			elapsed_since(&start));

	close_groups();

	return 0;
}
