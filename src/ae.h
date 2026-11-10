/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_AE_H
#define DD_AE_H

#include <stdint.h>

#include "frontend.h"

/*
 * Auto exposure for the IMX708: sets the sensor's exposure time and
 * analogue gain so the centre-weighted mean luminance of the frame meets a
 * target. A new setting shows 2 frames after the frame in which it is
 * written; the frames before that are not measured.
 */
struct ae_status {
	double exposure_us, gain;	/* applied */
	double luma;			/* last mean luminance, 0..1 */
	int automatic;
};

/*
 * subdev: the sensor node. exposure_us > 0: fixed exposure time and gain.
 * flicker_hz: mains frequency (50 or 60) for flicker-free times, or 0.
 */
int ae_open(const char *subdev, double fps, double exposure_us, double gain,
	    int flicker_hz);
void ae_close(void);
/* Statistics of the frame that started at ts_ns; gains: its white balance */
void ae_update(const struct meter *m, uint64_t ts_ns, const float gains[3]);
/* True when the exposure stopped changing, or is not automatic */
int ae_settled(void);
/* Returns -1 when there is no exposure control */
int ae_get(struct ae_status *s);

#endif
