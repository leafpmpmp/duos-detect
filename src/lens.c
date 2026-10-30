// SPDX-License-Identifier: AGPL-3.0-or-later
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "lens.h"

static struct {
	int fd;
	int min, max;
	pthread_mutex_t lock;
} lens = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* Lens drivers known to work: dw9807-vcm (also the DW9817) */
static int is_lens(const char *name)
{
	return !strncmp(name, "dw98", 4);
}

int lens_open(void)
{
	struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	char path[300], name[64];

	while (d && lens.fd < 0 && (e = readdir(d))) {
		FILE *f;

		if (strncmp(e->d_name, "v4l-subdev", 10))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && is_lens(name)) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			lens.fd = open(path, O_RDWR | O_CLOEXEC);
		}
		fclose(f);
	}
	if (d)
		closedir(d);
	if (lens.fd < 0)
		return -1;
	if (ioctl(lens.fd, VIDIOC_QUERYCTRL, &q)) {
		perror("lens focus control");
		lens_close();
		return -1;
	}
	lens.min = q.minimum;
	lens.max = q.maximum;
	return 0;
}

void lens_close(void)
{
	if (lens.fd >= 0)
		close(lens.fd);
	lens.fd = -1;
}

int lens_get(int *value, int *min, int *max)
{
	struct v4l2_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	int ret = -1;

	pthread_mutex_lock(&lens.lock);
	if (lens.fd >= 0 && !ioctl(lens.fd, VIDIOC_G_CTRL, &c)) {
		*value = c.value;
		*min = lens.min;
		*max = lens.max;
		ret = 0;
	}
	pthread_mutex_unlock(&lens.lock);
	return ret;
}

int lens_set(int value)
{
	struct v4l2_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	int ret = -1;

	pthread_mutex_lock(&lens.lock);
	if (lens.fd >= 0) {
		c.value = value < lens.min ? lens.min : value > lens.max ? lens.max : value;
		ret = ioctl(lens.fd, VIDIOC_S_CTRL, &c);
	}
	pthread_mutex_unlock(&lens.lock);
	return ret;
}
