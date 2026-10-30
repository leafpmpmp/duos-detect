// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdio.h>
#include <time.h>

#include "af.h"
#include "lens.h"

#define SETTLE		2	/* frames to skip after a lens move */
#define COARSE		48	/* steps of the first pass */
#define FINE		12	/* steps of the second pass, over +-COARSE */
#define START		160	/* the CM3 lens does not move below ~256 */

enum state { IDLE, COARSE_PASS, FINE_PASS };

static struct {
	volatile int request;	/* set by af_start() */
	volatile enum state st;
	int pos, end, step, skip, best_pos, steps, min, max;
	uint64_t best;
	double t0;
} af;

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void af_start(void)
{
	af.request = 1;
}

int af_busy(void)
{
	return af.request || af.st != IDLE;
}

/*
 * Sharpness of the central half of the frame: squared differences between
 * neighbouring pixels of the same colour (2 pixels apart), across and down,
 * from the MSB bytes. Any Bayer order works.
 */
static uint64_t sharpness(const uint8_t *raw, int width, int height, int stride)
{
	int groups = width / 2;			/* 3-byte groups per row */
	int g0 = groups / 4, g1 = groups * 3 / 4;
	uint64_t sum = 0;

	for (int y = height / 4; y < height * 3 / 4 - 2; y += 2) {
		const uint8_t *r = raw + (size_t)y * stride, *d = r + 2 * stride;

		for (int k = g0; k < g1; k++) {
			const uint8_t *p = r + 3 * k, *q = d + 3 * k;
			int h0 = p[3] - p[0], h1 = p[4] - p[1];
			int v0 = q[0] - p[0], v1 = q[1] - p[1];

			sum += h0 * h0 + h1 * h1 + v0 * v0 + v1 * v1;
		}
	}
	return sum;
}

static void move(int pos)
{
	af.pos = pos;
	af.skip = SETTLE;
	af.steps++;
	lens_set(pos);
}

void af_frame(const uint8_t *raw, int width, int height, int stride)
{
	uint64_t s;
	int v;

	if (af.request && af.st == IDLE) {
		af.request = 0;
		if (lens_get(&v, &af.min, &af.max))
			return;		/* no lens */
		af.st = COARSE_PASS;
		af.best = 0;
		af.steps = 0;
		af.t0 = now_ms();
		af.step = COARSE;
		af.end = af.max;
		move(af.min > START ? af.min : START);
		return;
	}
	if (af.st == IDLE)
		return;
	if (af.skip) {
		af.skip--;
		return;
	}

	s = sharpness(raw, width, height, stride);
	if (s > af.best) {
		af.best = s;
		af.best_pos = af.pos;
	}
	if (af.pos + af.step <= af.end) {
		move(af.pos + af.step);
	} else if (af.st == COARSE_PASS) {
		int lo = af.best_pos - COARSE, hi = af.best_pos + COARSE;

		af.st = FINE_PASS;
		af.step = FINE;
		af.end = hi > af.max ? af.max : hi;
		move(lo < af.min ? af.min : lo);
	} else {
		lens_set(af.best_pos);
		fprintf(stderr, "autofocus: position %d after %d steps, %.0f ms\n",
			af.best_pos, af.steps, now_ms() - af.t0);
		af.st = IDLE;
	}
}
