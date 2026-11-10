/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_AF_H
#define DD_AF_H

#include <stdint.h>

/*
 * Contrast autofocus. af_start() asks for a focus run (from any thread);
 * the capture thread passes every raw frame to af_frame(), which moves the
 * lens and measures sharpness until the run ends: a coarse pass over the
 * whole range, then a fine pass around the best position. It measures the
 * central half of the frame.
 */
void af_start(void);
/*
 * The same over a window of a quarter of the frame width and height around
 * x, y (fractions of the frame). A request while a run is busy starts a new
 * run.
 */
void af_start_at(float x, float y);
int af_busy(void);
/* Packed RAW12 frame, width x height pixels, stride bytes per row */
void af_frame(const uint8_t *raw, int width, int height, int stride);

#endif
