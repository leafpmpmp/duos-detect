/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_PREVIEW_H
#define DD_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#include "detector.h"

enum preview_mode {
	PREVIEW_AUTO,		/* the fastest route for this CPU */
	PREVIEW_RGB,		/* libjpeg converts RGB (SIMD on ARM) */
	PREVIEW_YCC,		/* our scalar YCbCr, libjpeg raw input */
	PREVIEW_YCC_XTV,	/* our XTheadVector YCbCr, libjpeg raw input */
};

/*
 * JPEG of the detector input with the boxes drawn in. rgb: [3][h][w] int8
 * planes R G B as the front end writes them (0..127, gamma encoded); the
 * boxes are drawn into it. w and h multiples of 16. Returns a malloc'ed
 * JPEG (free() it) and its size, or NULL.
 */
unsigned char *preview_jpeg(int8_t *rgb, int w, int h,
			    const struct det_box *box, int n, int quality,
			    enum preview_mode mode, unsigned long *size);

#endif
