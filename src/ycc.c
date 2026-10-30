// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Fixed-point steps, shared with ycc_xtv.c:
 *   8-bit value  v8 = 2v + (v >> 6)            (v: 0..127 -> 0..255)
 *   Y  = (77 R + 150 G + 29 B + 128) >> 8
 *   2x2 chroma average from the 7-bit sum s of 4 samples: (s + 1) >> 1
 *   Cb = ((-43 R - 85 G + 128 B + 128) >> 8) + 128
 *   Cr = ((128 R - 107 G - 21 B + 128) >> 8) + 128
 */
#include "ycc.h"

static inline int v8(int8_t v)
{
	return (v << 1) + (v >> 6);
}

void ycc420(const int8_t *r, const int8_t *g, const int8_t *b, int w, int h,
	    uint8_t *y, uint8_t *cb, uint8_t *cr)
{
	for (int i = 0; i < w * h; i++)
		y[i] = (77 * v8(r[i]) + 150 * v8(g[i]) + 29 * v8(b[i]) + 128) >> 8;

	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++) {
			int o = 2 * j * w + 2 * i;
			int rs = r[o] + r[o + 1] + r[o + w] + r[o + w + 1];
			int gs = g[o] + g[o + 1] + g[o + w] + g[o + w + 1];
			int bs = b[o] + b[o + 1] + b[o + w] + b[o + w + 1];
			int R = (rs + 1) >> 1, G = (gs + 1) >> 1, B = (bs + 1) >> 1;

			cb[j * (w / 2) + i] = ((-43 * R - 85 * G + 128 * B + 128) >> 8) + 128;
			cr[j * (w / 2) + i] = ((128 * R - 107 * G - 21 * B + 128) >> 8) + 128;
		}
}
