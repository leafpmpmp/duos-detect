/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_YCC_H
#define DD_YCC_H

#include <stdint.h>

/*
 * Planar RGB (int8 0..127, as the front end writes it) to JPEG YCbCr 4:2:0:
 * Y w x h, Cb and Cr (w/2) x (h/2). w and h even. Integer BT.601 full
 * range after a colour matrix; the vector version gives the same bytes.
 */
struct ycc_coef {
	int16_t y[3], cb[3], cr[3];	/* R G B weights, see ycc.c */
};

/*
 * ccm: camera RGB to sRGB, row by row, or NULL. Returns -1 when the matrix
 * is too large for the fixed-point steps.
 */
int ycc_coef(struct ycc_coef *k, const float *ccm);
void ycc420(const int8_t *r, const int8_t *g, const int8_t *b, int w, int h,
	    const struct ycc_coef *k, uint8_t *y, uint8_t *cb, uint8_t *cr);
#ifdef __riscv
void ycc420_xtv(const int8_t *r, const int8_t *g, const int8_t *b, int w,
		int h, const struct ycc_coef *k, uint8_t *y, uint8_t *cb,
		uint8_t *cr);
#endif

#endif
