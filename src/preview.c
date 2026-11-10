// SPDX-License-Identifier: AGPL-3.0-or-later
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include <jpeglib.h>

#include "cpu.h"
#include "preview.h"
#include "ycc.h"

#define LINE		2	/* box line width, pixels */
#define RGB_FRAC	4	/* fraction bits of the colour matrix terms */

/* Green box outline into the int8 RGB planes */
static void draw_box(int8_t *rgb, int w, int h, const struct det_box *b)
{
	size_t plane = (size_t)w * h;
	int x1 = b->x1 < 0 ? 0 : (int)b->x1, y1 = b->y1 < 0 ? 0 : (int)b->y1;
	int x2 = b->x2 >= w ? w - 1 : (int)b->x2;
	int y2 = b->y2 >= h ? h - 1 : (int)b->y2;

	for (int y = y1; y <= y2; y++)
		for (int x = x1; x <= x2; x++) {
			size_t i = (size_t)y * w + x;

			if (y >= y1 + LINE && y <= y2 - LINE &&
			    x >= x1 + LINE && x <= x2 - LINE) {
				x = x2 - LINE;	/* skip the inside */
				continue;
			}
			rgb[i] = 0;
			rgb[plane + i] = 127;
			rgb[2 * plane + i] = 0;
		}
}

/*
 * Colour matrix weights for 7-bit samples and 8-bit results. A term is
 * ((v << 7) k + 2^14) >> 15, as NEON vqrdmulh; the result is the sum of
 * three terms >> RGB_FRAC, rounded and clamped, as NEON vqrshrun.
 */
static int rgb_coef(int16_t k[9], const float *ccm)
{
	for (int i = 0; i < 3; i++) {
		float sum = 0;

		for (int c = 0; c < 3; c++) {
			float m = ccm ? ccm[3 * i + c] : i == c;

			sum += fabsf(m);
			k[3 * i + c] = lroundf(m * 255 / 127 * (1 << (8 + RGB_FRAC)));
		}
		if (sum > 3.9f)		/* the sum must fit 16 bits */
			return -1;
	}
	return 0;
}

static inline int term(int v, int k)
{
	return (2 * (v << 7) * k + (1 << 15)) >> 16;
}

/* Planar 7-bit RGB to packed 8-bit RGB, through the colour matrix */
static void pack_rgb(uint8_t *px, const int8_t *rgb, size_t plane,
		     const int16_t k[9])
{
	size_t i = 0;

#ifdef __aarch64__
	for (; i + 8 <= plane; i += 8) {
		int16x8_t v[3];
		uint8x8x3_t o;

		for (int j = 0; j < 3; j++)
			v[j] = vshll_n_s8(vld1_s8(rgb + j * plane + i), 7);
		for (int j = 0; j < 3; j++) {
			int16x8_t s = vqrdmulhq_n_s16(v[0], k[3 * j]);

			s = vaddq_s16(s, vqrdmulhq_n_s16(v[1], k[3 * j + 1]));
			s = vaddq_s16(s, vqrdmulhq_n_s16(v[2], k[3 * j + 2]));
			o.val[j] = vqrshrun_n_s16(s, RGB_FRAC);
		}
		vst3_u8(px + 3 * i, o);
	}
#endif
	for (; i < plane; i++)
		for (int j = 0; j < 3; j++) {
			int s = term(rgb[i], k[3 * j]) +
				term(rgb[plane + i], k[3 * j + 1]) +
				term(rgb[2 * plane + i], k[3 * j + 2]);

			s = (s + (1 << (RGB_FRAC - 1))) >> RGB_FRAC;
			px[3 * i + j] = s < 0 ? 0 : s > 255 ? 255 : s;
		}
}

/* Packed RGB rows for libjpeg's own colour conversion */
static int write_rgb(struct jpeg_compress_struct *c, const int8_t *rgb,
		     const float *ccm)
{
	int w = c->image_width, h = c->image_height;
	size_t plane = (size_t)w * h;
	uint8_t *px = malloc(plane * 3);
	int16_t k[9];

	if (!px)
		return -1;
	if (rgb_coef(k, ccm))
		rgb_coef(k, NULL);
	pack_rgb(px, rgb, plane, k);
	jpeg_start_compress(c, TRUE);
	while (c->next_scanline < c->image_height) {
		JSAMPROW row = px + (size_t)c->next_scanline * w * 3;

		jpeg_write_scanlines(c, &row, 1);
	}
	free(px);
	return 0;
}

/* Our YCbCr 4:2:0 planes as libjpeg raw input, 16 rows at a time */
static int write_ycc(struct jpeg_compress_struct *c, const int8_t *rgb,
		     const float *ccm, int xtv)
{
	int w = c->image_width, h = c->image_height;
	size_t plane = (size_t)w * h;
	uint8_t *y = malloc(plane + plane / 2), *cb = y + plane, *cr = cb + plane / 4;
	JSAMPROW yr[16], cbr[8], crr[8];
	JSAMPARRAY planes[3] = { yr, cbr, crr };
	struct ycc_coef k;

	if (!y)
		return -1;
	if (ycc_coef(&k, ccm))
		ycc_coef(&k, NULL);
#ifdef __riscv
	if (xtv)
		ycc420_xtv(rgb, rgb + plane, rgb + 2 * plane, w, h, &k, y, cb, cr);
	else
#endif
		ycc420(rgb, rgb + plane, rgb + 2 * plane, w, h, &k, y, cb, cr);
	(void)xtv;

	c->raw_data_in = TRUE;
	jpeg_set_colorspace(c, JCS_YCbCr);	/* Y 2x2, Cb and Cr 1x1 */
	jpeg_start_compress(c, TRUE);
	for (int row = 0; row < h; row += 16) {
		for (int i = 0; i < 16; i++)
			yr[i] = y + (size_t)(row + i) * w;
		for (int i = 0; i < 8; i++) {
			cbr[i] = cb + (size_t)(row / 2 + i) * (w / 2);
			crr[i] = cr + (size_t)(row / 2 + i) * (w / 2);
		}
		jpeg_write_raw_data(c, planes, 16);
	}
	free(y);
	return 0;
}

unsigned char *preview_jpeg(int8_t *rgb, int w, int h,
			    const struct det_box *box, int n, int quality,
			    enum preview_mode mode, const float *ccm,
			    unsigned long *size)
{
	struct jpeg_compress_struct c;
	struct jpeg_error_mgr err;
	unsigned char *out = NULL;
	int ret;

	if (w % 16 || h % 16)
		return NULL;
	/* libjpeg converts RGB with SIMD on ARM, not on RISC-V */
	if (mode == PREVIEW_AUTO)
#ifdef __riscv
		mode = cpu_xtheadvector() ? PREVIEW_YCC_XTV : PREVIEW_YCC;
#else
		mode = PREVIEW_RGB;
#endif
	if (mode == PREVIEW_YCC_XTV && !cpu_xtheadvector())
		return NULL;
	for (int i = 0; i < n; i++)
		draw_box(rgb, w, h, &box[i]);

	c.err = jpeg_std_error(&err);
	jpeg_create_compress(&c);
	*size = 0;
	jpeg_mem_dest(&c, &out, size);
	c.image_width = w;
	c.image_height = h;
	c.input_components = 3;
	c.in_color_space = mode == PREVIEW_RGB ? JCS_RGB : JCS_YCbCr;
	jpeg_set_defaults(&c);
	jpeg_set_quality(&c, quality, TRUE);
	c.dct_method = JDCT_IFAST;
	ret = mode == PREVIEW_RGB ? write_rgb(&c, rgb, ccm) :
	      write_ycc(&c, rgb, ccm, mode == PREVIEW_YCC_XTV);
	if (!ret)
		jpeg_finish_compress(&c);
	jpeg_destroy_compress(&c);
	if (ret) {
		free(out);
		return NULL;
	}
	return out;
}
