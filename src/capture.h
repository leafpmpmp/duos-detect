/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_CAPTURE_H
#define DD_CAPTURE_H

#include <stdint.h>

/* V4L2 capture with cached (non-coherent) MMAP buffers */
struct capture;

struct cap_frame {
	int index;
	const uint8_t *data;
	uint32_t sequence;
	uint64_t ts_ns;		/* CLOCK_MONOTONIC, start of frame */
};

struct capture *cap_open(const char *dev, int nbuf);
void cap_close(struct capture *cap);
void cap_format(const struct capture *cap, int *w, int *h, int *stride,
		uint32_t *fourcc);
int cap_start(struct capture *cap);
int cap_stop(struct capture *cap);
int cap_dequeue(struct capture *cap, struct cap_frame *f);
int cap_queue(struct capture *cap, int index);

#endif
