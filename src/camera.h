/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_CAMERA_H
#define DD_CAMERA_H

#include <stdint.h>

/*
 * Camera pipeline setup through the media controller: sensor -> CSI-2
 * receiver -> VI bridge -> capture node. It flips the image when the sensor
 * reports a 180 degree mounting, picks a sensor mode, sets every format
 * along the pipeline and the frame rate (vertical blanking).
 */
struct cam_setup {
	int width, height;	/* sensor mode; 0: the best fit for in_w x in_h */
	int in_w, in_h;		/* detector input (frame / 4, padded or cropped) */
	double fps;		/* 0: the fastest the mode allows */
};

struct cam_info {
	char sensor[64];	/* entity name */
	char video[64];		/* capture node, e.g. /dev/video0 */
	int width, height;
	double fps;
	int flipped;
};

int camera_setup(const struct cam_setup *s, struct cam_info *info);

#endif
