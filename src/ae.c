// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The exposure value (exposure time x gain) follows the ratio of target to
 * measured luminance on a log scale. The "normal" mode of the Raspberry Pi
 * tuning splits it: longer time first, then more gain. With a mains
 * frequency set, a time longer than one flicker period becomes a whole
 * number of periods, so mains lighting does not band the image.
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include "ae.h"
#include "imx708_tuning.h"

/*
 * Mean luminance target, linear. The Raspberry Pi tuning sets 0.16, which
 * its tone curve turns into 0.486; gamma 2.2 needs 0.20 for that output.
 */
#define TARGET		0.20
#define TRIM		0.1	/* part of the weight to drop: the brightest zones */
#define DELAY		2	/* frames until a written setting takes effect */
#define SPEED		0.5	/* part of each change to apply, log scale */
#define DEADBAND	0.05	/* leave changes below 5 % */
#define MAX_STEP	8.0	/* largest change per update */
#define NSTAGE		(sizeof(imx708_exposure_us) / sizeof(imx708_exposure_us[0]))

static struct {
	int fd;
	pthread_mutex_t lock;
	double line_us, frame_ns;
	int lines_min, lines_max, lines_step, code_max;
	int lines, code;	/* applied */
	int flicker_hz, startup, settled;
	uint32_t valid_from;	/* first frame with the applied setting */
	struct ae_status st;
} ae = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* IMX708 analogue gain code */
static double gain_of(int code)
{
	return 1024.0 / (1024 - code);
}

static int ctrl_get(uint32_t id, int *v)
{
	struct v4l2_control c = { .id = id };

	if (ioctl(ae.fd, VIDIOC_G_CTRL, &c))
		return -1;
	*v = c.value;
	return 0;
}

static int64_t pixel_rate(void)
{
	struct v4l2_ext_control c = { .id = V4L2_CID_PIXEL_RATE };
	struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL,
					.count = 1, .controls = &c };

	return ioctl(ae.fd, VIDIOC_G_EXT_CTRLS, &cs) ? -1 : c.value64;
}

/* Write a time and gain in one call. Returns 1 when nothing changes. */
static int apply(double us, double gain)
{
	int lines = (int)lround(us / ae.line_us / ae.lines_step) * ae.lines_step;
	int code = (int)lround(1024 - 1024 / gain);
	struct v4l2_ext_control c[2] = {
		{ .id = V4L2_CID_EXPOSURE }, { .id = V4L2_CID_ANALOGUE_GAIN },
	};
	struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL,
					.count = 2, .controls = c };

	lines = lines < ae.lines_min ? ae.lines_min :
		lines > ae.lines_max ? ae.lines_max : lines;
	code = code < 0 ? 0 : code > ae.code_max ? ae.code_max : code;
	if (lines == ae.lines && code == ae.code)
		return 1;
	c[0].value = lines;
	c[1].value = code;
	if (ioctl(ae.fd, VIDIOC_S_EXT_CTRLS, &cs)) {
		perror("exposure");
		return -1;
	}
	ae.lines = lines;
	ae.code = code;
	pthread_mutex_lock(&ae.lock);
	ae.st.exposure_us = lines * ae.line_us;
	ae.st.gain = gain_of(code);
	pthread_mutex_unlock(&ae.lock);
	return 0;
}

/* Exposure time and gain for an exposure value (us x gain) */
static void split(double ev, double *us, double *gain)
{
	double t_max = ae.lines_max * ae.line_us, g_max = gain_of(ae.code_max);
	double t = imx708_exposure_us[0], g = imx708_exposure_gain[0];

	for (size_t i = 1; i < NSTAGE && t * g < ev; i++) {
		double ti = imx708_exposure_us[i] < t_max ? imx708_exposure_us[i] : t_max;
		double gi = imx708_exposure_gain[i] < g_max ? imx708_exposure_gain[i] : g_max;

		if (ti * g >= ev) {
			t = ev / g;
			break;
		}
		t = ti;
		if (t * gi >= ev) {
			g = ev / t;
			break;
		}
		g = gi;
	}
	if (t * g > ev)		/* below the first stage */
		t = ev / g;
	/* Whole flicker periods, while the gain can make up the difference */
	if (ae.flicker_hz) {
		double period = 1e6 / (2 * ae.flicker_hz);
		double tf = floor(t / period) * period;

		if (tf >= period && ev / tf <= g_max) {
			t = tf;
			g = ev / t < 1 ? 1 : ev / t;
		}
	}
	*us = t;
	*gain = g;
}

struct zone_y {
	double y, w;
};

static int brighter(const void *a, const void *b)
{
	double ya = ((const struct zone_y *)a)->y, yb = ((const struct zone_y *)b)->y;

	return ya > yb ? -1 : ya < yb;
}

/*
 * Centre-weighted mean luminance after white balance, clipped like the
 * output. The brightest zones up to TRIM of the weight do not count: lamps
 * and small windows would make the rest of the frame dark.
 */
static double luminance(const struct meter *m, const float gains[3])
{
	struct zone_y z[METER_ZX * METER_ZY];
	double sum = 0, wsum = 0, skip;
	int n = 0;

	for (int j = 0; j < METER_ZY; j++)
		for (int i = 0; i < METER_ZX; i++) {
			const struct meter_zone *mz = &m->z[j][i];
			/* Distance from the centre, 1 at the middle of an edge */
			double dx = 2 * ((i + 0.5) / METER_ZX - 0.5);
			double dy = 2 * ((j + 0.5) / METER_ZY - 0.5);
			double d2 = dx * dx + dy * dy, k;

			if (!mz->n)
				continue;
			k = 1 / (mz->n * m->scale);
			z[n].y = 0.299 * fmin(mz->r * gains[0] * k, 1) +
				 0.587 * fmin(mz->g * gains[1] * k / 2, 1) +
				 0.114 * fmin(mz->b * gains[2] * k, 1);
			z[n].w = d2 < 0.4 * 0.4 ? 4 : d2 < 0.8 * 0.8 ? 2 : 1;
			wsum += z[n++].w;
		}
	qsort(z, n, sizeof(*z), brighter);
	skip = TRIM * wsum;
	wsum = 0;
	for (int k = 0; k < n; k++) {
		double w = z[k].w;

		if (skip > 0) {		/* part of a zone at the boundary */
			double cut = skip < w ? skip : w;

			skip -= cut;
			w -= cut;
		}
		sum += w * z[k].y;
		wsum += w;
	}
	return wsum ? sum / wsum : 0;
}

int ae_open(const char *subdev, double fps, double exposure_us, double gain,
	    int flicker_hz)
{
	struct v4l2_queryctrl qe = { .id = V4L2_CID_EXPOSURE };
	struct v4l2_queryctrl qg = { .id = V4L2_CID_ANALOGUE_GAIN };
	struct v4l2_subdev_format f = { .which = V4L2_SUBDEV_FORMAT_ACTIVE };
	int hb;
	int64_t rate;

	ae.fd = open(subdev, O_RDWR | O_CLOEXEC);
	if (ae.fd < 0) {
		perror(subdev);
		return -1;
	}
	if (ioctl(ae.fd, VIDIOC_QUERYCTRL, &qe) || ioctl(ae.fd, VIDIOC_QUERYCTRL, &qg) ||
	    ioctl(ae.fd, VIDIOC_SUBDEV_G_FMT, &f) || ctrl_get(V4L2_CID_HBLANK, &hb) ||
	    ctrl_get(V4L2_CID_EXPOSURE, &ae.lines) ||
	    ctrl_get(V4L2_CID_ANALOGUE_GAIN, &ae.code) || (rate = pixel_rate()) <= 0) {
		perror("exposure controls");
		ae_close();
		return -1;
	}
	ae.line_us = (f.format.width + hb) * 1e6 / rate;
	ae.frame_ns = 1e9 / fps;
	ae.lines_min = qe.minimum;
	ae.lines_max = qe.maximum;
	ae.lines_step = qe.step > 0 ? qe.step : 1;
	ae.code_max = qg.maximum;
	ae.flicker_hz = flicker_hz;
	ae.startup = 1;
	ae.st = (struct ae_status){
		.exposure_us = ae.lines * ae.line_us, .gain = gain_of(ae.code),
		.automatic = exposure_us <= 0,
	};
	if (exposure_us > 0 && apply(exposure_us, gain) < 0) {
		ae_close();
		return -1;
	}
	return 0;
}

void ae_close(void)
{
	if (ae.fd >= 0)
		close(ae.fd);
	ae.fd = -1;
}

void ae_update(const struct meter *m, uint64_t ts_ns, const float gains[3])
{
	double y, ratio, us, gain;
	struct timespec now;
	int ret;

	if (ae.fd < 0 || !ae.st.automatic || (int32_t)(m->seq - ae.valid_from) < 0)
		return;
	y = luminance(m, gains);
	pthread_mutex_lock(&ae.lock);
	ae.st.luma = y;
	pthread_mutex_unlock(&ae.lock);

	ratio = TARGET / (y > 1e-4 ? y : 1e-4);
	ratio = ratio > MAX_STEP ? MAX_STEP : ratio < 1 / MAX_STEP ? 1 / MAX_STEP : ratio;
	if (fabs(log(ratio)) < log(1 + DEADBAND)) {
		ae.settled = 1;
		ae.startup = 0;
		return;
	}
	split(ae.st.exposure_us * ae.st.gain * pow(ratio, ae.startup ? 1 : SPEED),
	      &us, &gain);
	ret = apply(us, gain);
	ae.settled = ret != 0;	/* at a limit, or the write failed */
	if (ret) {
		ae.startup = 0;
		return;
	}
	/* The write lands in the frame now running: count from that one */
	clock_gettime(CLOCK_MONOTONIC, &now);
	ae.valid_from = m->seq + DELAY +
		(uint32_t)((now.tv_sec * 1e9 + now.tv_nsec - ts_ns) / ae.frame_ns);
}

int ae_settled(void)
{
	return ae.fd < 0 || !ae.st.automatic || ae.settled;
}

int ae_get(struct ae_status *s)
{
	if (ae.fd < 0)
		return -1;
	pthread_mutex_lock(&ae.lock);
	*s = ae.st;
	pthread_mutex_unlock(&ae.lock);
	return 0;
}
