// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

#include "cpu.h"
#include "preview.h"
#include "ycc.h"

#define LINE	2	/* box line width, pixels */

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

/* Packed RGB rows for libjpeg's own colour conversion */
static int write_rgb(struct jpeg_compress_struct *c, const int8_t *rgb)
{
	static uint8_t to8[256];
	int w = c->image_width, h = c->image_height;
	size_t plane = (size_t)w * h;
	uint8_t *px = malloc(plane * 3);

	if (!px)
		return -1;
	if (!to8[127])
		for (int v = 0; v < 128; v++)
			to8[v] = (v * 255 + 63) / 127;	/* negative: 0 */
	for (size_t i = 0; i < plane; i++) {
		px[i * 3] = to8[(uint8_t)rgb[i]];
		px[i * 3 + 1] = to8[(uint8_t)rgb[plane + i]];
		px[i * 3 + 2] = to8[(uint8_t)rgb[2 * plane + i]];
	}
	jpeg_start_compress(c, TRUE);
	while (c->next_scanline < c->image_height) {
		JSAMPROW row = px + (size_t)c->next_scanline * w * 3;

		jpeg_write_scanlines(c, &row, 1);
	}
	free(px);
	return 0;
}

/* Our YCbCr 4:2:0 planes as libjpeg raw input, 16 rows at a time */
static int write_ycc(struct jpeg_compress_struct *c, const int8_t *rgb, int xtv)
{
	int w = c->image_width, h = c->image_height;
	size_t plane = (size_t)w * h;
	uint8_t *y = malloc(plane + plane / 2), *cb = y + plane, *cr = cb + plane / 4;
	JSAMPROW yr[16], cbr[8], crr[8];
	JSAMPARRAY planes[3] = { yr, cbr, crr };

	if (!y)
		return -1;
#ifdef __riscv
	if (xtv)
		ycc420_xtv(rgb, rgb + plane, rgb + 2 * plane, w, h, y, cb, cr);
	else
#endif
		ycc420(rgb, rgb + plane, rgb + 2 * plane, w, h, y, cb, cr);
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
			    enum preview_mode mode, unsigned long *size)
{
	struct jpeg_compress_struct c;
	struct jpeg_error_mgr err;
	unsigned char *out = NULL;
	int ret;

	if (w % 16 || h % 16)
		return NULL;
	if (mode == PREVIEW_AUTO)
		mode = cpu_xtheadvector() ? PREVIEW_YCC_XTV : PREVIEW_RGB;
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
	ret = mode == PREVIEW_RGB ? write_rgb(&c, rgb) :
	      write_ycc(&c, rgb, mode == PREVIEW_YCC_XTV);
	if (!ret)
		jpeg_finish_compress(&c);
	jpeg_destroy_compress(&c);
	if (ret) {
		free(out);
		return NULL;
	}
	return out;
}
