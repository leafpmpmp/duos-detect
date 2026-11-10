// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * ycc420() on the C906 vector unit (XTheadVector), built with
 * -march=rv64gc_xtheadvector. Same fixed-point steps as ycc.c.
 */
#include <riscv_vector.h>
#include <stddef.h>

#include "ycc.h"

/* v - 64, as int16 */
static inline vint16m4_t centre(const int8_t *p, size_t vl)
{
	vuint8m2_t v = __riscv_vle8_v_u8m2((const uint8_t *)p, vl);

	return __riscv_vreinterpret_v_u16m4_i16m4(__riscv_vwsubu_vx_u16m4(v, 64, vl));
}

/* Sum of the 2x2 samples of one channel, as int16 */
static inline vint16m2_t sum4(const int8_t *p, size_t w, size_t vl)
{
	vuint8m1x2_t a = __riscv_vlseg2e8_v_u8m1x2((const uint8_t *)p, vl);
	vuint8m1x2_t b = __riscv_vlseg2e8_v_u8m1x2((const uint8_t *)p + w, vl);
	vuint16m2_t s = __riscv_vwaddu_vv_u16m2(__riscv_vget_v_u8m1x2_u8m1(a, 0),
						__riscv_vget_v_u8m1x2_u8m1(a, 1), vl);

	s = __riscv_vwaddu_wv_u16m2(s, __riscv_vget_v_u8m1x2_u8m1(b, 0), vl);
	s = __riscv_vwaddu_wv_u16m2(s, __riscv_vget_v_u8m1x2_u8m1(b, 1), vl);
	return __riscv_vreinterpret_v_u16m2_i16m2(s);
}

/* ((k . s + 8192) >> 14) + 128, clamped to uint8 */
static inline void chroma(vint16m2_t R, vint16m2_t G, vint16m2_t B,
			  const int16_t k[3], uint8_t *out, size_t vl)
{
	vint32m4_t a = __riscv_vmv_v_x_i32m4(8192, vl);
	vint16m2_t c;

	a = __riscv_vwmacc_vx_i32m4(a, k[0], R, vl);
	a = __riscv_vwmacc_vx_i32m4(a, k[1], G, vl);
	a = __riscv_vwmacc_vx_i32m4(a, k[2], B, vl);
	c = __riscv_vadd_vx_i16m2(__riscv_vnsra_wx_i16m2(a, 14, vl), 128, vl);
	c = __riscv_vmin_vx_i16m2(__riscv_vmax_vx_i16m2(c, 0, vl), 255, vl);
	__riscv_vse8_v_u8m1(out, __riscv_vncvt_x_x_w_u8m1(
				    __riscv_vreinterpret_v_i16m2_u16m2(c), vl), vl);
}

void ycc420_xtv(const int8_t *r, const int8_t *g, const int8_t *b, int w,
		int h, const struct ycc_coef *k, uint8_t *y, uint8_t *cb,
		uint8_t *cr)
{
	size_t n = (size_t)w * h;

	for (size_t i = 0, vl; i < n; i += vl) {
		vint16m4_t a;

		vl = __riscv_vsetvl_e8m2(n - i);
		a = __riscv_vmul_vx_i16m4(centre(r + i, vl), k->y[0], vl);
		a = __riscv_vmacc_vx_i16m4(a, k->y[1], centre(g + i, vl), vl);
		a = __riscv_vmacc_vx_i16m4(a, k->y[2], centre(b + i, vl), vl);
		a = __riscv_vsra_vx_i16m4(__riscv_vadd_vx_i16m4(a, 128, vl), 7, vl);
		a = __riscv_vadd_vx_i16m4(a, 128, vl);
		a = __riscv_vmin_vx_i16m4(__riscv_vmax_vx_i16m4(a, 0, vl), 255, vl);
		__riscv_vse8_v_u8m2(y + i, __riscv_vncvt_x_x_w_u8m2(
					    __riscv_vreinterpret_v_i16m4_u16m4(a), vl), vl);
	}

	for (int j = 0; j < h / 2; j++) {
		size_t row = (size_t)2 * j * w, half = w / 2;

		for (size_t i = 0, vl; i < half; i += vl) {
			size_t o = row + 2 * i;
			vint16m2_t R, G, B;

			vl = __riscv_vsetvl_e8m1(half - i);
			R = sum4(r + o, w, vl);
			G = sum4(g + o, w, vl);
			B = sum4(b + o, w, vl);
			chroma(R, G, B, k->cb, cb + j * half + i, vl);
			chroma(R, G, B, k->cr, cr + j * half + i, vl);
		}
	}
}
