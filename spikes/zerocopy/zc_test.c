// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Zero-copy test: feed V4L2 camera buffers to the TPU by physical address.
 *
 * Model A reads each frame in place (VIDIOC_EXPBUF -> CVITPU_GET_DMABUF_PADDR
 * -> CVI_NN_SetTensorPhysicalAddr). Model B gets a CPU copy of the same
 * bytes. The outputs of A and B must be identical.
 *
 *   zc_test [-c] [-x] [-n frames] [-d /dev/videoN] model.cvimodel
 *
 * -c requests cached (non-coherent) buffers. -x is a negative control:
 * model A reads the wrong buffer, so the outputs must differ.
 * Set the media pipeline formats before the run (see run.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <cviruntime.h>

/* From cviruntime src/soc/common/linux/bm_npu_ioctl.h (mainline port) */
struct cvi_dmabuf_paddr_arg {
	int fd;
	unsigned int reserved;
	unsigned long long paddr;
};
#define CVITPU_GET_DMABUF_PADDR _IOWR('p', 0x0E, unsigned long long)
#define CVITPU_RELEASE_DMABUF	_IOW('p', 0x0F, unsigned long long)

#define NBUF 4

struct buf {
	void *vaddr;
	size_t len;
	int dmafd;
	uint64_t paddr;
};

struct net {
	CVI_MODEL_HANDLE model;
	CVI_TENSOR *in, *out;
	int32_t nin, nout;
};

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static void xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int ret;

	do
		ret = ioctl(fd, req, arg);
	while (ret < 0 && errno == EINTR);
	if (ret < 0)
		die(what);
}

static void load(struct net *n, const char *path)
{
	if (CVI_NN_RegisterModel(path, &n->model) != CVI_RC_SUCCESS) {
		fprintf(stderr, "cannot load %s\n", path);
		exit(1);
	}
	if (CVI_NN_GetInputOutputTensors(n->model, &n->in, &n->nin,
					 &n->out, &n->nout) != CVI_RC_SUCCESS) {
		fprintf(stderr, "cannot get tensors\n");
		exit(1);
	}
}

static double forward(struct net *n)
{
	double t = now_ms();

	if (CVI_NN_Forward(n->model, n->in, n->nin, n->out, n->nout) !=
	    CVI_RC_SUCCESS) {
		fprintf(stderr, "forward failed\n");
		exit(1);
	}
	return now_ms() - t;
}

/* Return the number of differing output bytes between two nets */
static size_t compare(struct net *a, struct net *b)
{
	size_t diff = 0;

	for (int i = 0; i < a->nout; i++) {
		const uint8_t *pa = CVI_NN_TensorPtr(&a->out[i]);
		const uint8_t *pb = CVI_NN_TensorPtr(&b->out[i]);
		size_t len = CVI_NN_TensorSize(&a->out[i]);

		for (size_t j = 0; j < len; j++)
			diff += pa[j] != pb[j];
	}
	return diff;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	struct buf bufs[NBUF];
	struct net za, cp;
	int frames = 30, cached = 0, wrong = 0, opt;
	double t_zc = 0, t_cp = 0, t_copy = 0;
	size_t bad_frames = 0, insize;

	while ((opt = getopt(argc, argv, "cxn:d:")) != -1) {
		switch (opt) {
		case 'c': cached = 1; break;
		case 'x': wrong = 1; break;
		case 'n': frames = atoi(optarg); break;
		case 'd': dev = optarg; break;
		default: goto usage;
		}
	}
	if (optind != argc - 1) {
usage:
		fprintf(stderr, "usage: %s [-c] [-x] [-n frames] [-d dev] model\n",
			argv[0]);
		return 1;
	}

	load(&za, argv[optind]);
	load(&cp, argv[optind]);
	insize = CVI_NN_TensorSize(&za.in[0]);

	int vfd = open(dev, O_RDWR);
	int tfd = open("/dev/cvi-tpu0", O_RDWR);

	if (vfd < 0 || tfd < 0)
		die("open");

	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

	xioctl(vfd, VIDIOC_G_FMT, &fmt, "G_FMT");
	printf("frame %ux%u stride %u size %u, model input %zu bytes\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage, insize);
	if (insize > fmt.fmt.pix.sizeimage) {
		fprintf(stderr, "model input larger than a frame\n");
		return 1;
	}

	struct v4l2_requestbuffers req = {
		.count = NBUF,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.flags = cached ? V4L2_MEMORY_FLAG_NON_COHERENT : 0,
	};

	xioctl(vfd, VIDIOC_REQBUFS, &req, "REQBUFS");
	if (req.count != NBUF) {
		fprintf(stderr, "got %u buffers\n", req.count);
		return 1;
	}
	if (cached && !(req.flags & V4L2_MEMORY_FLAG_NON_COHERENT))
		fprintf(stderr, "warning: driver ignored the non-coherent flag\n");

	for (int i = 0; i < NBUF; i++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		struct v4l2_exportbuffer exp = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.index = i,
			.flags = O_RDWR | O_CLOEXEC,
		};
		struct cvi_dmabuf_paddr_arg pa = { 0 };

		xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF");
		bufs[i].len = b.length;
		bufs[i].vaddr = mmap(NULL, b.length, PROT_READ, MAP_SHARED,
				     vfd, b.m.offset);
		if (bufs[i].vaddr == MAP_FAILED)
			die("mmap");
		xioctl(vfd, VIDIOC_EXPBUF, &exp, "EXPBUF");
		bufs[i].dmafd = exp.fd;
		pa.fd = exp.fd;
		xioctl(tfd, CVITPU_GET_DMABUF_PADDR, &pa, "GET_DMABUF_PADDR");
		bufs[i].paddr = pa.paddr;
		printf("buf %d: %zu bytes at paddr 0x%llx\n", i, bufs[i].len,
		       (unsigned long long)bufs[i].paddr);
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF");
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	xioctl(vfd, VIDIOC_STREAMON, &type, "STREAMON");

	for (int f = 0; f < frames; f++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		double t;
		size_t diff;

		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF");

		CVI_NN_SetTensorPhysicalAddr(&za.in[0],
					     bufs[(b.index + wrong) % NBUF].paddr);
		t_zc += forward(&za);

		t = now_ms();
		memcpy(CVI_NN_TensorPtr(&cp.in[0]), bufs[b.index].vaddr, insize);
		t_copy += now_ms() - t;
		t_cp += forward(&cp);

		diff = compare(&za, &cp);
		if (diff) {
			bad_frames++;
			printf("frame %u (buf %u): %zu output bytes differ\n",
			       b.sequence, b.index, diff);
		}
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF");
	}

	xioctl(vfd, VIDIOC_STREAMOFF, &type, "STREAMOFF");

	printf("%d frames, %zu mismatched (%s buffers)\n", frames, bad_frames,
	       cached ? "cached" : "uncached");
	printf("forward zero-copy %.2f ms, forward copy %.2f ms, memcpy %zu B %.2f ms\n",
	       t_zc / frames, t_cp / frames, insize, t_copy / frames);

	for (int i = 0; i < NBUF; i++) {
		ioctl(tfd, CVITPU_RELEASE_DMABUF, &bufs[i].dmafd);
		close(bufs[i].dmafd);
		munmap(bufs[i].vaddr, bufs[i].len);
	}
	close(tfd);
	close(vfd);
	CVI_NN_CleanupModel(za.model);
	CVI_NN_CleanupModel(cp.model);
	return bad_frames ? 2 : 0;
}
