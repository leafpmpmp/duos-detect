// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * ycc420() on the C906 vector unit (XTheadVector), built with
 * -march=rv64gc_xtheadvector. Same fixed-point steps as ycc.c.
 */
#include <riscv_vector.h>
#include <stddef.h>

#include "ycc.h"

/* 7-bit -> 8-bit: 2v + (v >> 6) */
static inline vuint8m2_t to8(const int8_t *p, size_t vl)
{
	vuint8m2_t v = __riscv_vle8_v_u8m2((const uint8_t *)p, vl);

	return __riscv_vadd_vv_u8m2(__riscv_vsll_vx_u8m2(v, 1, vl),
				    __riscv_vsrl_vx_u8m2(v, 6, vl), vl);
}

/* 2x2 average of one channel: (sum of 4 + 1) >> 1, as int16 */
static inline vint16m2_t avg4(const int8_t *p, size_t w, size_t vl)
{
	vuint8m1x2_t a = __riscv_vlseg2e8_v_u8m1x2((const uint8_t *)p, vl);
	vuint8m1x2_t b = __riscv_vlseg2e8_v_u8m1x2((const uint8_t *)p + w, vl);
	vuint16m2_t s = __riscv_vwaddu_vv_u16m2(__riscv_vget_v_u8m1x2_u8m1(a, 0),
						__riscv_vget_v_u8m1x2_u8m1(a, 1), vl);

	s = __riscv_vwaddu_wv_u16m2(s, __riscv_vget_v_u8m1x2_u8m1(b, 0), vl);
	s = __riscv_vwaddu_wv_u16m2(s, __riscv_vget_v_u8m1x2_u8m1(b, 1), vl);
	s = __riscv_vsrl_vx_u16m2(__riscv_vadd_vx_u16m2(s, 1, vl), 1, vl);
	return __riscv_vreinterpret_v_u16m2_i16m2(s);
}

/* ((kr R + kg G + kb B + 128) >> 8) + 128, narrowed to uint8 */
static inline void chroma(vint16m2_t R, vint16m2_t G, vint16m2_t B, int kr,
			  int kg, int kb, uint8_t *out, size_t vl)
{
	vint16m2_t a = __riscv_vmul_vx_i16m2(R, kr, vl);

	a = __riscv_vmacc_vx_i16m2(a, kg, G, vl);
	a = __riscv_vmacc_vx_i16m2(a, kb, B, vl);
	a = __riscv_vsra_vx_i16m2(__riscv_vadd_vx_i16m2(a, 128, vl), 8, vl);
	a = __riscv_vadd_vx_i16m2(a, 128, vl);
	__riscv_vse8_v_u8m1(out, __riscv_vncvt_x_x_w_u8m1(
				    __riscv_vreinterpret_v_i16m2_u16m2(a), vl), vl);
}

void ycc420_xtv(const int8_t *r, const int8_t *g, const int8_t *b, int w,
		int h, uint8_t *y, uint8_t *cb, uint8_t *cr)
{
	size_t n = (size_t)w * h;

	for (size_t i = 0, vl; i < n; i += vl) {
		vuint16m4_t acc;

		vl = __riscv_vsetvl_e8m2(n - i);
		acc = __riscv_vwmulu_vx_u16m4(to8(r + i, vl), 77, vl);
		acc = __riscv_vwmaccu_vx_u16m4(acc, 150, to8(g + i, vl), vl);
		acc = __riscv_vwmaccu_vx_u16m4(acc, 29, to8(b + i, vl), vl);
		acc = __riscv_vadd_vx_u16m4(acc, 128, vl);
		__riscv_vse8_v_u8m2(y + i, __riscv_vnsrl_wx_u8m2(acc, 8, vl), vl);
	}

	for (int j = 0; j < h / 2; j++) {
		size_t row = (size_t)2 * j * w, half = w / 2;

		for (size_t i = 0, vl; i < half; i += vl) {
			size_t o = row + 2 * i;
			vint16m2_t R, G, B;

			vl = __riscv_vsetvl_e8m1(half - i);
			R = avg4(r + o, w, vl);
			G = avg4(g + o, w, vl);
			B = avg4(b + o, w, vl);
			chroma(R, G, B, -43, -85, 128, cb + j * half + i, vl);
			chroma(R, G, B, 128, -107, -21, cr + j * half + i, vl);
		}
	}
}
