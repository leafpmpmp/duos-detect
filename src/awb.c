// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Grey world as in the Raspberry Pi AWB fallback: R/G and B/G per zone,
 * averaged over the middle half of the zones, so strong colours at both
 * ends drop out. The white point then moves to the nearest point of the
 * colour temperature curve, keeping a small distance off the curve.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "awb.h"
#include "imx708_tuning.h"

#define SPEED		0.2	/* part of each change to apply, log scale */
#define MIN_CELLS	4	/* unclipped cells for a zone to count */
#define MIN_G		0.01	/* darker zones do not count (linear G) */
#define MIN_ZONES	8
#define CHANGE		0.01	/* gain change that rebuilds the tables */
#define CT_STEP		10	/* search step along the curve, K */
#define NCT		(sizeof(imx708_ct_curve) / sizeof(imx708_ct_curve[0]))
#define NCCM		(sizeof(imx708_ccm) / sizeof(imx708_ccm[0]))

struct zone {
	double r, g, b;
};

static struct {
	pthread_mutex_t lock;
	double r, b;		/* white point, R/G and B/G of grey */
	int startup;
	struct awb_status st;
} awb = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Curve point at a temperature; straight lines past the ends */
static void curve(double ct, double *r, double *b)
{
	size_t i = 0;
	const double *p, *q;
	double f;

	while (i + 2 < NCT && ct > imx708_ct_curve[i + 1][0])
		i++;
	p = imx708_ct_curve[i];
	q = imx708_ct_curve[i + 1];
	f = (ct - p[0]) / (q[0] - p[0]);
	*r = p[1] + f * (q[1] - p[1]);
	*b = p[2] + f * (q[2] - p[2]);
}

/* The curve temperature nearest to a white point, from lo to hi */
static double nearest(double r, double b, double lo, double hi, double step)
{
	double best = INFINITY, ct = lo, cr, cb;

	for (double t = lo; t <= hi; t += step) {
		double d;

		curve(t, &cr, &cb);
		d = (cr - r) * (cr - r) + (cb - b) * (cb - b);
		if (d < best) {
			best = d;
			ct = t;
		}
	}
	return ct;
}

/* Move a white point onto the curve; returns its temperature */
static double project(double *r, double *b)
{
	double ct, cr, cb, pr, pb, tr, tb, n, off;

	ct = nearest(*r, *b, IMX708_CT_LO, IMX708_CT_HI, 10 * CT_STEP);
	ct = nearest(*r, *b, fmax(ct - 10 * CT_STEP, IMX708_CT_LO),
		     fmin(ct + 10 * CT_STEP, IMX708_CT_HI), CT_STEP);
	curve(ct, &cr, &cb);
	curve(ct - CT_STEP, &pr, &pb);
	curve(ct + CT_STEP, &tr, &tb);
	tr -= pr;
	tb -= pb;
	n = hypot(tr, tb);
	/* Distance along the normal (-tb, tr) */
	off = ((*r - cr) * -tb + (*b - cb) * tr) / n;
	off = off < -IMX708_TRANSVERSE ? -IMX708_TRANSVERSE :
	      off > IMX708_TRANSVERSE ? IMX708_TRANSVERSE : off;
	*r = cr - off * tb / n;
	*b = cb + off * tr / n;
	return ct;
}

static int by_r(const void *a, const void *b)
{
	const struct zone *x = a, *y = b;

	return x->r * y->g < y->r * x->g ? -1 : x->r * y->g > y->r * x->g;
}

static int by_b(const void *a, const void *b)
{
	const struct zone *x = a, *y = b;

	return x->b * y->g < y->b * x->g ? -1 : x->b * y->g > y->b * x->g;
}

void awb_init(const float gains[3], int automatic)
{
	awb.r = gains[1] / gains[0];
	awb.b = gains[1] / gains[2];
	awb.startup = 1;
	awb.st.automatic = automatic;
	memcpy(awb.st.gains, gains, sizeof(awb.st.gains));
	{
		double r = awb.r, b = awb.b;

		awb.st.ct = project(&r, &b);
	}
}

int awb_update(const struct meter *m, float gains[3])
{
	struct zone zr[METER_ZX * METER_ZY], zb[METER_ZX * METER_ZY];
	double sr = 0, sgr = 0, sb = 0, sgb = 0, r, b, k, ct;
	float g0, g2;
	int n = 0, changed;

	if (!awb.st.automatic)
		return 0;
	for (int j = 0; j < METER_ZY; j++)
		for (int i = 0; i < METER_ZX; i++) {
			const struct meter_zone *z = &m->z[j][i];

			/* A cell has 2 G samples */
			if (z->un < MIN_CELLS || z->ug < MIN_G * 2 * z->un * m->scale)
				continue;
			zr[n++] = (struct zone){ z->ur, z->ug / 2.0, z->ub };
		}
	if (n < MIN_ZONES)
		return 0;	/* too dark or too bright to tell */
	memcpy(zb, zr, n * sizeof(*zr));
	qsort(zr, n, sizeof(*zr), by_r);
	qsort(zb, n, sizeof(*zb), by_b);
	for (int i = n / 4; i < n - n / 4; i++) {
		sr += zr[i].r;
		sgr += zr[i].g;
		sb += zb[i].b;
		sgb += zb[i].g;
	}
	if (sr <= 0 || sb <= 0)
		return 0;
	r = sr / sgr;
	b = sb / sgb;
	ct = project(&r, &b);

	k = awb.startup ? 1 : SPEED;
	awb.startup = 0;
	awb.r *= pow(r / awb.r, k);
	awb.b *= pow(b / awb.b, k);
	g0 = 1 / awb.r;
	g2 = 1 / awb.b;
	changed = fabsf(g0 / gains[0] - 1) > CHANGE || fabsf(g2 / gains[2] - 1) > CHANGE;

	pthread_mutex_lock(&awb.lock);
	awb.st.ct += k * (ct - awb.st.ct);
	if (changed) {
		gains[0] = g0;
		gains[1] = 1;
		gains[2] = g2;
		memcpy(awb.st.gains, gains, sizeof(awb.st.gains));
	}
	pthread_mutex_unlock(&awb.lock);
	return changed;
}

void awb_get(struct awb_status *s)
{
	pthread_mutex_lock(&awb.lock);
	*s = awb.st;
	pthread_mutex_unlock(&awb.lock);
}

/* Straight lines between the table matrices; the end matrices past the ends */
void awb_ccm(double ct, float ccm[9])
{
	const double (*t)[10] = imx708_ccm;
	size_t i = 0;
	double f;

	while (i + 2 < NCCM && ct > t[i + 1][0])
		i++;
	f = (ct - t[i][0]) / (t[i + 1][0] - t[i][0]);
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	for (int k = 1; k < 10; k++)
		ccm[k - 1] = t[i][k] + f * (t[i + 1][k] - t[i][k]);
}
