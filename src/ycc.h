/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_YCC_H
#define DD_YCC_H

#include <stdint.h>

/*
 * Planar RGB (int8 0..127, as the front end writes it) to JPEG YCbCr 4:2:0:
 * Y w x h, Cb and Cr (w/2) x (h/2). w and h even. Integer BT.601 full
 * range; the vector version gives the same bytes.
 */
void ycc420(const int8_t *r, const int8_t *g, const int8_t *b, int w, int h,
	    uint8_t *y, uint8_t *cb, uint8_t *cr);
#ifdef __riscv
void ycc420_xtv(const int8_t *r, const int8_t *g, const int8_t *b, int w,
		int h, uint8_t *y, uint8_t *cb, uint8_t *cr);
#endif

#endif
