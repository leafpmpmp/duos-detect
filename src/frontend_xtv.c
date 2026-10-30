// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Front end row kernel for the C906 vector unit (XTheadVector, the RVV 0.7.1
 * draft), built with -march=rv64gc_xtheadvector. Same math as the scalar code
 * in frontend.c.
 *
 * A 6-field segment load splits each 6-byte group of a raw row (4 pixels:
 * MSB MSB LSB MSB MSB LSB) into one register per byte position. The lookup
 * tables are 16-bit: with XTheadVector the index of an indexed load has the
 * element width of the data, so 8-bit data cannot take 16-bit indices.
 */
#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

#include "frontend_xtv.h"

/* Sum of the even (f = 0) or odd (f = 1) pixel MSBs of two raw rows */
#define SUM(a, b, f)							\
	__riscv_vwaddu_wv_u16m2(__riscv_vwaddu_wv_u16m2(		\
		__riscv_vwaddu_vv_u16m2(__riscv_vget_v_u8m1x6_u8m1(a, f),	\
					__riscv_vget_v_u8m1x6_u8m1(a, f + 3), vl), \
		__riscv_vget_v_u8m1x6_u8m1(b, f), vl),			\
		__riscv_vget_v_u8m1x6_u8m1(b, f + 3), vl)

static inline vuint16m2_t pick(int k, vuint16m2_t s0, vuint16m2_t s1,
			       vuint16m2_t s2, vuint16m2_t s3)
{
	return k == 0 ? s0 : k == 1 ? s1 : k == 2 ? s2 : s3;
}

/* Table lookup: 16-bit entries, byte offsets = 2 * index */
static inline void lookup(const int16_t *lut, vuint16m2_t idx, int8_t *out,
			  size_t vl)
{
	vint16m2_t q = __riscv_vluxei16_v_i16m2(lut,
			__riscv_vsll_vx_u16m2(idx, 1, vl), vl);

	__riscv_vse8_v_i8m1(out, __riscv_vncvt_x_x_w_i8m1(q, vl), vl);
}

void fe_row_xtv(const uint8_t *r0, size_t stride, size_t n,
		const int16_t *lut_r, const int16_t *lut_g, const int16_t *lut_b,
		const int pos[4], int8_t *R, int8_t *G, int8_t *B)
{
	const uint8_t *r1 = r0 + stride, *r2 = r1 + stride, *r3 = r2 + stride;

	for (size_t x = 0, vl; x < n; x += vl) {
		size_t o = x * 6;
		vuint8m1x6_t a, b;
		vuint16m2_t s00, s01, s10, s11;

		vl = __riscv_vsetvl_e8m1(n - x);
		a = __riscv_vlseg6e8_v_u8m1x6(r0 + o, vl);
		b = __riscv_vlseg6e8_v_u8m1x6(r2 + o, vl);
		s00 = SUM(a, b, 0);
		s01 = SUM(a, b, 1);
		a = __riscv_vlseg6e8_v_u8m1x6(r1 + o, vl);
		b = __riscv_vlseg6e8_v_u8m1x6(r3 + o, vl);
		s10 = SUM(a, b, 0);
		s11 = SUM(a, b, 1);

		lookup(lut_r, pick(pos[0], s00, s01, s10, s11), R + x, vl);
		lookup(lut_g, __riscv_vadd_vv_u16m2(pick(pos[1], s00, s01, s10, s11),
						    pick(pos[2], s00, s01, s10, s11), vl),
		       G + x, vl);
		lookup(lut_b, pick(pos[3], s00, s01, s10, s11), B + x, vl);
	}
}
