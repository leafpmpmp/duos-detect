// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The weights are BT.601 times the colour matrix, for 7-bit samples v
 * (0..127 stands for 0..255). Fixed-point steps, shared with ycc_xtv.c:
 *   Y  = ((ky . (v - 64) + 128) >> 7) + 128     16-bit sum
 *   Cb = ((kb . s + 8192) >> 14) + 128          32-bit sum, s: 2x2 sum of v
 *   Cr = ((kr . s + 8192) >> 14) + 128
 * Each result is clamped to 0..255. The Y weights sum to 257 and the chroma
 * weights to 0, so grey stays grey.
 *
 * The matrix is for linear RGB, but the samples are gamma encoded. Near
 * grey, both give the same result.
 */
#include <math.h>
#include <stdlib.h>

#include "ycc.h"

static const float bt601[3][3] = {
	{ 0.299f, 0.587f, 0.114f },
	{ -0.168736f, -0.331264f, 0.5f },
	{ 0.5f, -0.418688f, -0.081312f },
};

/* Round the weights; the largest one takes the rounding error of the sum */
static void fit(int16_t k[3], const float f[3], float scale, int sum)
{
	int big = 0;

	for (int c = 0; c < 3; c++) {
		k[c] = lroundf(f[c] * scale);
		sum -= k[c];
		if (abs(k[c]) > abs(k[big]))
			big = c;
	}
	k[big] += sum;
}

int ycc_coef(struct ycc_coef *k, const float *ccm)
{
	float m[3][3] = { 0 }, ysum = 0, cmax = 0;

	for (int i = 0; i < 3; i++)
		for (int c = 0; c < 3; c++)
			for (int j = 0; j < 3; j++)
				m[i][c] += bt601[i][j] * (ccm ? ccm[3 * j + c] : j == c);
	for (int c = 0; c < 3; c++) {
		ysum += fabsf(m[0][c]);
		cmax = fmaxf(cmax, fmaxf(fabsf(m[1][c]), fabsf(m[2][c])));
	}
	/* The Y sum must fit 16 bits, each chroma weight too */
	if (ysum > 1.9f || cmax > 3.9f)
		return -1;
	fit(k->y, m[0], 255.0f / 127 * 128, 257);
	fit(k->cb, m[1], 255.0f / 508 * 16384, 0);
	fit(k->cr, m[2], 255.0f / 508 * 16384, 0);
	return 0;
}

static inline uint8_t clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

void ycc420(const int8_t *r, const int8_t *g, const int8_t *b, int w, int h,
	    const struct ycc_coef *k, uint8_t *y, uint8_t *cb, uint8_t *cr)
{
	for (int i = 0; i < w * h; i++) {
		int a = k->y[0] * (r[i] - 64) + k->y[1] * (g[i] - 64) +
			k->y[2] * (b[i] - 64);

		y[i] = clamp8(((a + 128) >> 7) + 128);
	}

	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++) {
			int o = 2 * j * w + 2 * i, c = j * (w / 2) + i;
			int rs = r[o] + r[o + 1] + r[o + w] + r[o + w + 1];
			int gs = g[o] + g[o + 1] + g[o + w] + g[o + w + 1];
			int bs = b[o] + b[o + 1] + b[o + w] + b[o + w + 1];

			cb[c] = clamp8(((k->cb[0] * rs + k->cb[1] * gs +
					 k->cb[2] * bs + 8192) >> 14) + 128);
			cr[c] = clamp8(((k->cr[0] * rs + k->cr[1] * gs +
					 k->cr[2] * bs + 8192) >> 14) + 128);
		}
}
