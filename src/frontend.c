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
#define GAMMA_N		16384	/* steps of the gamma table over 0..1 */
#define METER_STEP	4	/* metering: every 4th block of a row */
#define METER_ROWS	8	/* in every 8th output row */
#define CLIP		250	/* MSB value of a clipped sample */

struct frontend {
	struct fe_params p;
	int pos[4];		/* Bayer position (0..3) of R, G1, G2, B */
	int8_t lut[3][8 * 255 + 1];
	int8_t pad;		/* int8 value of the padding */
#ifdef __riscv
	int xtv;		/* use the vector unit */
	int16_t lut16[3][8 * 255 + 1];	/* same, for XTheadVector */
#endif
	int black;		/* MSB black level, for the statistics */
	int meter_off[4];	/* byte offsets of R, G1, G2, B in a 2x2 cell */
	int meter_n;		/* metered cells per row */
	uint8_t meter_zone[256];	/* zone column of each cell */
	int8_t gam[GAMMA_N + 1];	/* linear 0..1 -> int8 output */
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

			v = v < 0 ? 0 : v > 1 ? 1 : v;
			fe->lut[c][s] = fe->gam[lroundf(v * GAMMA_N)];
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
	/* White balance changes rebuild the tables: keep powf out of that */
	for (int i = 0; i <= GAMMA_N; i++) {
		long q = lroundf(powf((float)i / GAMMA_N, 1 / p->gamma) * p->out_scale);

		fe->gam[i] = q > 127 ? 127 : q;
	}
	fe_set_gains(fe, p->gains);
	fe->pad = (int8_t)lroundf(p->pad_value * p->out_scale);
#ifdef __riscv
	fe->xtv = !p->no_simd && cpu_xtheadvector();
#endif
	fe->black = (int)lroundf(p->black);
	for (int c = 0; c < 4; c++)
		fe->meter_off[c] = (fe->pos[c] >> 1) * p->stride + (fe->pos[c] & 1);
	fe->meter_n = (p->out_w - 2 * p->pad) / METER_STEP;
	if (fe->meter_n > (int)sizeof(fe->meter_zone))
		fe->meter_n = sizeof(fe->meter_zone);
	for (int i = 0; i < fe->meter_n; i++)
		fe->meter_zone[i] = i * METER_ZX / fe->meter_n;
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

/* Value above black, else 0. No branch: the C906 has no conditional move. */
static inline uint32_t above(int v, int black)
{
	int d = v - black;

	return d & ~(d >> 31);
}

static inline void zone_add(struct meter_zone *z, const struct meter_zone *a)
{
	z->r += a->r;
	z->g += a->g;
	z->b += a->b;
	z->n += a->n;
	z->ur += a->ur;
	z->ug += a->ug;
	z->ub += a->ub;
	z->un += a->un;
}

/*
 * One 2x2 cell per 16x32 pixels, from the raw rows of an output row.
 * The sums of a zone stay in registers until the next zone starts. Kept
 * out of fe_run: inlined, it slows the NEON row loop by 7%.
 */
static __attribute__((noinline)) void meter_row(const struct frontend *fe,
						const uint8_t *r0, int y,
						struct meter *m)
{
	const int black = fe->black, *off = fe->meter_off;
	struct meter_zone *zr = m->z[y * METER_ZY / fe->p.out_h], a = { 0 };
	int zone = fe->meter_zone[0];

	for (int i = 0; i < fe->meter_n; i++) {
		const uint8_t *q = r0 + (i * METER_STEP + METER_STEP / 2) * 6;
		int vr = q[off[0]], vg1 = q[off[1]], vg2 = q[off[2]], vb = q[off[3]];
		uint32_t r = above(vr, black), b = above(vb, black);
		uint32_t g = above(vg1, black) + above(vg2, black);
		uint32_t ok = (vr < CLIP) & (vg1 < CLIP) & (vg2 < CLIP) & (vb < CLIP);

		if (fe->meter_zone[i] != zone) {
			zone_add(&zr[zone], &a);
			a = (struct meter_zone){ 0 };
			zone = fe->meter_zone[i];
		}
		a.r += r;
		a.g += g;
		a.b += b;
		a.n++;
		a.ur += r & -ok;
		a.ug += g & -ok;
		a.ub += b & -ok;
		a.un += ok;
	}
	zone_add(&zr[zone], &a);
}

void fe_run(const struct frontend *fe, const uint8_t *raw, int8_t *out,
	    struct meter *m)
{
	const struct fe_params *p = &fe->p;
	const int n = p->out_w - 2 * p->pad, plane = p->out_w * p->out_h;
	uint16_t buf[4][n];
	uint16_t *s[4] = { buf[0], buf[1], buf[2], buf[3] };
	const uint16_t *sr = s[fe->pos[0]], *sg1 = s[fe->pos[1]],
		       *sg2 = s[fe->pos[2]], *sb = s[fe->pos[3]];

	if (m) {
		memset(m->z, 0, sizeof(m->z));
		m->scale = p->white - p->black;
	}
	for (int y = 0; y < p->out_h; y++) {
		const uint8_t *r0 = raw + (size_t)(y + p->top) * BIN * p->stride +
				    p->left * 6;
		int8_t *R = out + y * p->out_w, *G = R + plane, *B = G + plane;

		/* Statistics from the raw rows of this output row */
		if (m && y % METER_ROWS == METER_ROWS / 2)
			meter_row(fe, r0, y, m);
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
