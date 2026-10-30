/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_FRONTEND_XTV_H
#define DD_FRONTEND_XTV_H

#include <stddef.h>
#include <stdint.h>

/*
 * One output row of n blocks from four raw rows, on the C906 vector unit.
 * pos: Bayer position (0..3) of R, G1, G2, B. Tables: 16-bit entries.
 */
void fe_row_xtv(const uint8_t *r0, size_t stride, size_t n,
		const int16_t *lut_r, const int16_t *lut_g, const int16_t *lut_b,
		const int pos[4], int8_t *R, int8_t *G, int8_t *B);

#endif
