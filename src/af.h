/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_AF_H
#define DD_AF_H

#include <stdint.h>

/*
 * Contrast autofocus. af_start() asks for a focus run (from any thread);
 * the capture thread passes every raw frame to af_frame(), which moves the
 * lens and measures sharpness until the run ends: a coarse pass over the
 * whole range, then a fine pass around the best position.
 */
void af_start(void);
int af_busy(void);
/* Packed RAW12 frame, width x height pixels, stride bytes per row */
void af_frame(const uint8_t *raw, int width, int height, int stride);

#endif
