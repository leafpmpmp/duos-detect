/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_DETECTOR_H
#define DD_DETECTOR_H

#include <stdint.h>

/*
 * YOLO detector on the TPU (Ultralytics detect head, cut at the per-stride
 * box and class convs, int8 input and outputs). Each slot is a clone of
 * the model with its own input and output buffers: the CPU can fill one
 * slot while the TPU runs another.
 */
struct det_box {
	float x1, y1, x2, y2;	/* model input pixels */
	float score;
	int cls;
};

struct detector;

struct detector *det_open(const char *model, int nslots);
void det_close(struct detector *det);
void det_input_size(const struct detector *det, int *w, int *h);
float det_input_scale(const struct detector *det);
int det_classes(const struct detector *det);
/* [3][h][w] int8, planes R G B */
int8_t *det_input(struct detector *det, int slot);
int det_run(struct detector *det, int slot);
/* Boxes of class cls with score >= conf after NMS; returns the count */
int det_decode(struct detector *det, int slot, int cls, float conf,
	       float iou, struct det_box *out, int max);

#endif
