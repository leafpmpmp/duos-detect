/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_FRONTEND_H
#define DD_FRONTEND_H

#include <stdint.h>

/*
 * Raw camera front end: packed RAW12 Bayer frame -> 4x4-binned planar RGB,
 * written as the int8 input of the detector.
 */
struct fe_params {
	int width, height, stride;	/* raw frame, stride in bytes */
	const char *bayer;		/* "BGGR", "RGGB", "GBRG" or "GRBG" */
	int out_w, out_h;		/* output size */
	int left, top;			/* crop offset, in output pixels */
	int pad;			/* grey columns on each side */
	float pad_value;		/* their value, 0..1 */
	float black, white;		/* 8-bit MSB scale */
	float gains[3];			/* R G B */
	float gamma;
	float out_scale;		/* int8 = value (0..1) * out_scale */
	int no_simd;			/* force the scalar code (tests) */
};

/*
 * Zone statistics of a raw frame over the detector's field of view, for
 * auto exposure and white balance: sums over one 2x2 Bayer cell per 16x32
 * pixels, MSB values above black. Each cell adds 1 R, 2 G and 1 B sample.
 */
#define METER_ZX	16
#define METER_ZY	12

struct meter_zone {
	uint32_t r, g, b, n;		/* all cells */
	uint32_t ur, ug, ub, un;	/* only the cells without a clipped sample */
};

struct meter {
	struct meter_zone z[METER_ZY][METER_ZX];
	float scale;			/* white - black: the sample value of 1.0 */
	uint32_t seq;			/* frame sequence number, set by the caller */
};

struct frontend;

struct frontend *fe_create(const struct fe_params *p);
void fe_destroy(struct frontend *fe);
/* Rebuild the lookup tables for new white balance gains (R G B) */
void fe_set_gains(struct frontend *fe, const float gains[3]);
/* out: [3][out_h][out_w] int8, planes R G B; m: statistics too, or NULL */
void fe_run(const struct frontend *fe, const uint8_t *raw, int8_t *out,
	    struct meter *m);
/* The code path fe_run takes: "NEON", "XTheadVector" or "scalar" */
const char *fe_simd(const struct frontend *fe);

#endif
