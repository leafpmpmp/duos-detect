/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_LENS_H
#define DD_LENS_H

/*
 * Focus lens (VCM) of the camera module, a V4L2 lens subdevice such as the
 * DW9817 of the Camera Module 3. The driver powers the lens only while the
 * subdevice is open, so lens_open() keeps it open until lens_close().
 */
int lens_open(void);
void lens_close(void);
/* Position and range; -1 without a lens */
int lens_get(int *value, int *min, int *max);
int lens_set(int value);

#endif
