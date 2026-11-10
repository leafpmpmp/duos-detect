/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_AWB_H
#define DD_AWB_H

#include "frontend.h"

/*
 * Auto white balance: grey world over the frame zones, moved onto the
 * colour temperature curve of the IMX708.
 */
struct awb_status {
	float gains[3];		/* R G B */
	double ct;		/* colour temperature of the white point, K */
	int automatic;
};

/* gains: the start values, or the fixed gains when automatic is 0 */
void awb_init(const float gains[3], int automatic);
/* Updates gains; returns 1 when they changed enough to rebuild the tables */
int awb_update(const struct meter *m, float gains[3]);
void awb_get(struct awb_status *s);
/* Colour matrix at a temperature: camera RGB to sRGB, row by row */
void awb_ccm(double ct, float ccm[9]);

#endif
