// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Each output pixel sums a 4x4 Bayer block from the MSB bytes of the packed
 * RAW12 data (the LSB bytes are skipped): 4 samples for each of the two
 * Bayer positions of one colour row parity, so per block S00 S01 S10 S11 of
 * 4 samples each. R and B use one position, G uses two (8 samples).
 * Per-channel lookup tables then apply black level, white balance,
 * normalisation, gamma and the int8 input scale of the detector.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#ifdef __riscv
#include "frontend_xtv.h"
#endif

#include "cpu.h"
#include "frontend.h"

#define BIN 4

struct frontend {
	struct fe_params p;
	int pos[4];		/* Bayer position (0..3) of R, G1, G2, B */
	int8_t lut[3][8 * 255 + 1];
	int8_t pad;		/* int8 value of the padding */
#ifdef __riscv
	int xtv;		/* use the vector unit */
	int16_t lut16[3][8 * 255 + 1];	/* same, for XTheadVector */
#endif
};

const char *fe_simd(const struct frontend *fe)
{
#ifdef __aarch64__
	if (!fe->p.no_simd)
		return "NEON";
#endif
#ifdef __riscv
	if (fe->xtv)
		return "XTheadVector";
#endif
	(void)fe;
	return "scalar";
}

static int bayer_positions(const char *bayer, int pos[4])
{
	int g = 0;

	if (!bayer || strlen(bayer) != 4)
		return -1;
	pos[0] = pos[3] = -1;
	for (int i = 0; i < 4; i++) {
		switch (bayer[i]) {
		case 'R': pos[0] = i; break;
		case 'B': pos[3] = i; break;
		case 'G':
			if (g < 2)
				pos[1 + g] = i;
			g++;
			break;
		default: return -1;
		}
	}
	return pos[0] < 0 || pos[3] < 0 || g != 2 ? -1 : 0;
}

void fe_set_gains(struct frontend *fe, const float gains[3])
{
	static const int n[3] = { 4, 8, 4 };	/* samples summed per channel */
	const struct fe_params *p = &fe->p;

	memcpy(fe->p.gains, gains, sizeof(fe->p.gains));
	for (int c = 0; c < 3; c++)
		for (int s = 0; s <= n[c] * 255; s++) {
			float v = ((float)s / n[c] - p->black) * gains[c] /
				  (p->white - p->black);
			long q;

			v = v < 0 ? 0 : v > 1 ? 1 : v;
			q = lroundf(powf(v, 1 / p->gamma) * p->out_scale);
			fe->lut[c][s] = q > 127 ? 127 : q;
#ifdef __riscv
			fe->lut16[c][s] = fe->lut[c][s];
#endif
		}
}

struct frontend *fe_create(const struct fe_params *p)
{
	struct frontend *fe;

	if (p->left < 0 || p->top < 0 || p->pad < 0 || p->out_w <= 2 * p->pad ||
	    (p->left + p->out_w - 2 * p->pad) * BIN > p->width ||
	    (p->top + p->out_h) * BIN > p->height)
		return NULL;
	fe = calloc(1, sizeof(*fe));
	if (!fe)
		return NULL;
	fe->p = *p;
	if (bayer_positions(p->bayer, fe->pos)) {
		free(fe);
		return NULL;
	}
	fe_set_gains(fe, p->gains);
	fe->pad = (int8_t)lroundf(p->pad_value * p->out_scale);
#ifdef __riscv
	fe->xtv = !p->no_simd && cpu_xtheadvector();
#endif
	return fe;
}

void fe_destroy(struct frontend *fe)
{
	free(fe);
}

/* Sum of the 4 samples of each Bayer position, blocks x0..n-1 (scalar) */
static void block_sums(const uint8_t *r0, int stride, int x0, int n,
		       uint16_t *s[4])
{
	const uint8_t *r1 = r0 + stride, *r2 = r1 + stride, *r3 = r2 + stride;

	for (int x = x0; x < n; x++) {
		int o = x * 6;	/* two 3-byte groups = 4 pixels */

		s[0][x] = r0[o] + r0[o + 3] + r2[o] + r2[o + 3];
		s[1][x] = r0[o + 1] + r0[o + 4] + r2[o + 1] + r2[o + 4];
		s[2][x] = r1[o] + r1[o + 3] + r3[o] + r3[o + 3];
		s[3][x] = r1[o + 1] + r1[o + 4] + r3[o + 1] + r3[o + 4];
	}
}

#ifdef __aarch64__
/*
 * One output row on NEON: per 16 blocks, sum in registers and look the
 * results up at once, while they are hot. Returns the blocks done.
 */
static int row_neon(const struct frontend *fe, const uint8_t *r0, int stride,
		    int n, int8_t *R, int8_t *G, int8_t *B)
{
	const uint8_t *r1 = r0 + stride, *r2 = r1 + stride, *r3 = r2 + stride;
	uint16_t sr[16], sg[16], sb[16];
	int x = 0;

	/* 16 blocks = 32 3-byte groups = 96 bytes per raw row */
	for (; x + 16 <= n; x += 16) {
		int o = x * 6;
		uint8x16x3_t a0 = vld3q_u8(r0 + o), b0 = vld3q_u8(r0 + o + 48);
		uint8x16x3_t a1 = vld3q_u8(r1 + o), b1 = vld3q_u8(r1 + o + 48);
		uint8x16x3_t a2 = vld3q_u8(r2 + o), b2 = vld3q_u8(r2 + o + 48);
		uint8x16x3_t a3 = vld3q_u8(r3 + o), b3 = vld3q_u8(r3 + o + 48);
		/* val[0]: even pixel MSBs, val[1]: odd pixel MSBs */
		uint16x8_t s[4][2] = {
			{ vpadalq_u8(vpaddlq_u8(a0.val[0]), a2.val[0]),
			  vpadalq_u8(vpaddlq_u8(b0.val[0]), b2.val[0]) },
			{ vpadalq_u8(vpaddlq_u8(a0.val[1]), a2.val[1]),
			  vpadalq_u8(vpaddlq_u8(b0.val[1]), b2.val[1]) },
			{ vpadalq_u8(vpaddlq_u8(a1.val[0]), a3.val[0]),
			  vpadalq_u8(vpaddlq_u8(b1.val[0]), b3.val[0]) },
			{ vpadalq_u8(vpaddlq_u8(a1.val[1]), a3.val[1]),
			  vpadalq_u8(vpaddlq_u8(b1.val[1]), b3.val[1]) },
		};
		const int *pos = fe->pos;

		vst1q_u16(sr, s[pos[0]][0]);
		vst1q_u16(sr + 8, s[pos[0]][1]);
		vst1q_u16(sg, vaddq_u16(s[pos[1]][0], s[pos[2]][0]));
		vst1q_u16(sg + 8, vaddq_u16(s[pos[1]][1], s[pos[2]][1]));
		vst1q_u16(sb, s[pos[3]][0]);
		vst1q_u16(sb + 8, s[pos[3]][1]);
		for (int i = 0; i < 16; i++) {
			R[x + i] = fe->lut[0][sr[i]];
			G[x + i] = fe->lut[1][sg[i]];
			B[x + i] = fe->lut[2][sb[i]];
		}
	}
	return x;
}
#endif

void fe_run(const struct frontend *fe, const uint8_t *raw, int8_t *out)
{
	const struct fe_params *p = &fe->p;
	const int n = p->out_w - 2 * p->pad, plane = p->out_w * p->out_h;
	uint16_t buf[4][n];
	uint16_t *s[4] = { buf[0], buf[1], buf[2], buf[3] };
	const uint16_t *sr = s[fe->pos[0]], *sg1 = s[fe->pos[1]],
		       *sg2 = s[fe->pos[2]], *sb = s[fe->pos[3]];

	for (int y = 0; y < p->out_h; y++) {
		const uint8_t *r0 = raw + (size_t)(y + p->top) * BIN * p->stride +
				    p->left * 6;
		int8_t *R = out + y * p->out_w, *G = R + plane, *B = G + plane;

		for (int c = 0; c < 3 && p->pad; c++) {
			memset(R + c * plane, fe->pad, p->pad);
			memset(R + c * plane + p->pad + n, fe->pad, p->pad);
		}
		R += p->pad;
		G += p->pad;
		B += p->pad;
#ifdef __riscv
		if (fe->xtv) {
			fe_row_xtv(r0, p->stride, n, fe->lut16[0], fe->lut16[1],
				   fe->lut16[2], fe->pos, R, G, B);
			continue;
		}
#endif
		int x0 = 0;

#ifdef __aarch64__
		if (!p->no_simd)
			x0 = row_neon(fe, r0, p->stride, n, R, G, B);
#endif
		block_sums(r0, p->stride, x0, n, s);
		for (int x = x0; x < n; x++) {
			R[x] = fe->lut[0][sr[x]];
			G[x] = fe->lut[1][sg1[x] + sg2[x]];
			B[x] = fe->lut[2][sb[x]];
		}
	}
}
