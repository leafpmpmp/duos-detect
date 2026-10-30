// SPDX-License-Identifier: AGPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "capture.h"

#define MAX_BUF 8

struct capture {
	int fd, nbuf;
	struct v4l2_pix_format pix;
	void *vaddr[MAX_BUF];
	size_t len[MAX_BUF];
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do
		ret = ioctl(fd, req, arg);
	while (ret < 0 && errno == EINTR);
	return ret;
}

void cap_close(struct capture *cap)
{
	if (!cap)
		return;
	for (int i = 0; i < cap->nbuf; i++)
		munmap(cap->vaddr[i], cap->len[i]);
	close(cap->fd);
	free(cap);
}

struct capture *cap_open(const char *dev, int nbuf)
{
	struct capture *cap = calloc(1, sizeof(*cap));
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers req = {
		.count = nbuf,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		/* Cached: uncached CPU reads are ~13x slower */
		.flags = V4L2_MEMORY_FLAG_NON_COHERENT,
	};

	if (!cap || nbuf > MAX_BUF)
		goto err;
	cap->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (cap->fd < 0) {
		perror(dev);
		goto err;
	}
	if (xioctl(cap->fd, VIDIOC_G_FMT, &fmt) ||
	    xioctl(cap->fd, VIDIOC_REQBUFS, &req)) {
		perror("capture setup");
		goto err;
	}
	if (!(req.flags & V4L2_MEMORY_FLAG_NON_COHERENT))
		fprintf(stderr, "warning: %s ignored the cached-buffer request\n",
			dev);
	cap->pix = fmt.fmt.pix;
	for (cap->nbuf = 0; cap->nbuf < (int)req.count; cap->nbuf++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = cap->nbuf,
		};

		if (xioctl(cap->fd, VIDIOC_QUERYBUF, &b))
			goto err;
		cap->len[cap->nbuf] = b.length;
		cap->vaddr[cap->nbuf] = mmap(NULL, b.length, PROT_READ,
					     MAP_SHARED, cap->fd, b.m.offset);
		if (cap->vaddr[cap->nbuf] == MAP_FAILED) {
			perror("mmap");
			goto err;
		}
	}
	return cap;
err:
	cap_close(cap);
	return NULL;
}

void cap_format(const struct capture *cap, int *w, int *h, int *stride,
		uint32_t *fourcc)
{
	*w = cap->pix.width;
	*h = cap->pix.height;
	*stride = cap->pix.bytesperline;
	*fourcc = cap->pix.pixelformat;
}

int cap_queue(struct capture *cap, int index)
{
	struct v4l2_buffer b = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
	};

	return xioctl(cap->fd, VIDIOC_QBUF, &b);
}

int cap_start(struct capture *cap)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (int i = 0; i < cap->nbuf; i++)
		if (cap_queue(cap, i))
			return -1;
	return xioctl(cap->fd, VIDIOC_STREAMON, &type);
}

int cap_stop(struct capture *cap)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
}

int cap_dequeue(struct capture *cap, struct cap_frame *f)
{
	struct v4l2_buffer b = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};

	/* DQBUF also invalidates the CPU cache for the buffer */
	if (xioctl(cap->fd, VIDIOC_DQBUF, &b))
		return -1;
	f->index = b.index;
	f->data = cap->vaddr[b.index];
	f->sequence = b.sequence;
	f->ts_ns = b.timestamp.tv_sec * 1000000000ull +
		   b.timestamp.tv_usec * 1000ull;
	return 0;
}
