// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * CPU front end test: packed RAW12 BGGR frame -> 4x4-binned planar RGB.
 *
 * Each output pixel sums a 4x4 Bayer block from the MSB bytes (4 B, 8 G,
 * 4 R samples). A per-channel lookup table then applies black level,
 * white balance, normalisation and gamma. Same math as tools/frontend.py.
 *
 *   cpufe [-n frames] [-s] [-u] [-d /dev/videoN] [-o dump-prefix]
 *
 * -s uses the scalar code instead of NEON. -u uses uncached buffers. -o writes the last raw frame
 * and its output for comparison with the Python reference.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#define W	2304
#define H	1296
#define BIN	4
#define OW	(W / BIN)		/* 576 */
#define OH	320			/* rows kept of H / BIN = 324 */
#define TOP	((H / BIN - OH) / 2)	/* 2 */
#define NBUF	4

static const float black = 16, white = 255, gamma_ = 2.2f;
static const float gains[3] = { 1.942f, 1.0f, 1.623f };	/* R G B */

/* lut[c][sum]: channel c (R G B), sum of n samples (n = 4, 8, 4) */
static uint8_t lut[3][8 * 255 + 1];

static void make_luts(void)
{
	static const int n[3] = { 4, 8, 4 };

	for (int c = 0; c < 3; c++)
		for (int s = 0; s <= n[c] * 255; s++) {
			float v = ((float)s / n[c] - black) * gains[c] /
				  (white - black);

			v = v < 0 ? 0 : v > 1 ? 1 : v;
			lut[c][s] = (uint8_t)(powf(v, 1 / gamma_) * 255 + 0.5f);
		}
}

/* Output row y from four raw rows: plane stride OW, planes R G B */
static void row_scalar(const uint8_t *r0, const uint8_t *r1,
		       const uint8_t *r2, const uint8_t *r3,
		       uint8_t *R, uint8_t *G, uint8_t *B)
{
	for (int x = 0; x < OW; x++) {
		int o = x * 6;	/* two 3-byte groups = 4 pixels */
		int b = r0[o] + r0[o + 3] + r2[o] + r2[o + 3];
		int g = r0[o + 1] + r0[o + 4] + r2[o + 1] + r2[o + 4] +
			r1[o] + r1[o + 3] + r3[o] + r3[o + 3];
		int r = r1[o + 1] + r1[o + 4] + r3[o + 1] + r3[o + 4];

		R[x] = lut[0][r];
		G[x] = lut[1][g];
		B[x] = lut[2][b];
	}
}

#ifdef __aarch64__
static void row_neon(const uint8_t *r0, const uint8_t *r1,
		     const uint8_t *r2, const uint8_t *r3,
		     uint8_t *R, uint8_t *G, uint8_t *B)
{
	uint16_t sr[16], sg[16], sb[16];

	/* 16 output pixels = 32 groups = 96 bytes per raw row */
	for (int x = 0; x < OW; x += 16) {
		int o = x * 6;
		uint8x16x3_t a0 = vld3q_u8(r0 + o), b0 = vld3q_u8(r0 + o + 48);
		uint8x16x3_t a1 = vld3q_u8(r1 + o), b1 = vld3q_u8(r1 + o + 48);
		uint8x16x3_t a2 = vld3q_u8(r2 + o), b2 = vld3q_u8(r2 + o + 48);
		uint8x16x3_t a3 = vld3q_u8(r3 + o), b3 = vld3q_u8(r3 + o + 48);
		/* val[0]: even pixel MSBs, val[1]: odd pixel MSBs */
		uint16x8_t bl = vpaddlq_u8(a0.val[0]), bh = vpaddlq_u8(b0.val[0]);
		uint16x8_t gl = vpaddlq_u8(a0.val[1]), gh = vpaddlq_u8(b0.val[1]);
		uint16x8_t rl = vpaddlq_u8(a1.val[1]), rh = vpaddlq_u8(b1.val[1]);

		bl = vpadalq_u8(bl, a2.val[0]);
		bh = vpadalq_u8(bh, b2.val[0]);
		gl = vpadalq_u8(gl, a2.val[1]);
		gh = vpadalq_u8(gh, b2.val[1]);
		gl = vpadalq_u8(gl, a1.val[0]);
		gh = vpadalq_u8(gh, b1.val[0]);
		gl = vpadalq_u8(gl, a3.val[0]);
		gh = vpadalq_u8(gh, b3.val[0]);
		rl = vpadalq_u8(rl, a3.val[1]);
		rh = vpadalq_u8(rh, b3.val[1]);

		vst1q_u16(sb, bl);
		vst1q_u16(sb + 8, bh);
		vst1q_u16(sg, gl);
		vst1q_u16(sg + 8, gh);
		vst1q_u16(sr, rl);
		vst1q_u16(sr + 8, rh);
		for (int i = 0; i < 16; i++) {
			R[x + i] = lut[0][sr[i]];
			G[x + i] = lut[1][sg[i]];
			B[x + i] = lut[2][sb[i]];
		}
	}
}
#endif

static void frontend(const uint8_t *raw, unsigned int stride, uint8_t *out,
		     int scalar)
{
	for (int y = 0; y < OH; y++) {
		const uint8_t *r0 = raw + (size_t)((y + TOP) * BIN) * stride;
		uint8_t *R = out + y * OW, *G = R + OH * OW, *B = G + OH * OW;

#ifdef __aarch64__
		if (!scalar) {
			row_neon(r0, r0 + stride, r0 + 2 * stride,
				 r0 + 3 * stride, R, G, B);
			continue;
		}
#endif
		row_scalar(r0, r0 + stride, r0 + 2 * stride, r0 + 3 * stride,
			   R, G, B);
	}
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int ret;

	do
		ret = ioctl(fd, req, arg);
	while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		perror(what);
		exit(1);
	}
}

static void dump(const char *prefix, const char *ext, const void *p, size_t n)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s.%s", prefix, ext);
	f = fopen(path, "wb");
	if (!f || fwrite(p, 1, n, f) != n) {
		perror(path);
		exit(1);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0", *prefix = NULL;
	int frames = 100, scalar = 0, uncached = 0, opt;
	static uint8_t out[3 * OH * OW];
	void *vaddr[NBUF];
	int dmafd[NBUF];
	double t_inv = 0, t_dq = 0, t_fe = 0, t_fe_max = 0;

	while ((opt = getopt(argc, argv, "n:sud:o:")) != -1) {
		switch (opt) {
		case 'n': frames = atoi(optarg); break;
		case 's': scalar = 1; break;
		case 'u': uncached = 1; break;
		case 'd': dev = optarg; break;
		case 'o': prefix = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-n frames] [-s] [-u] [-d dev] [-o prefix]\n",
				argv[0]);
			return 1;
		}
	}
	make_luts();

	int vfd = open(dev, O_RDWR);
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

	if (vfd < 0) {
		perror(dev);
		return 1;
	}
	xioctl(vfd, VIDIOC_G_FMT, &fmt, "G_FMT");
	if (fmt.fmt.pix.width != W || fmt.fmt.pix.height != H) {
		fprintf(stderr, "need %dx%d, got %ux%u\n", W, H,
			fmt.fmt.pix.width, fmt.fmt.pix.height);
		return 1;
	}

	struct v4l2_requestbuffers req = {
		.count = NBUF,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.flags = uncached ? 0 : V4L2_MEMORY_FLAG_NON_COHERENT,
	};

	xioctl(vfd, VIDIOC_REQBUFS, &req, "REQBUFS");
	for (int i = 0; i < NBUF; i++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF");
		struct v4l2_exportbuffer exp = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.index = i,
			.flags = O_RDONLY | O_CLOEXEC,
		};

		vaddr[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, vfd,
				b.m.offset);
		if (vaddr[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		xioctl(vfd, VIDIOC_EXPBUF, &exp, "EXPBUF");
		dmafd[i] = exp.fd;
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF");
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	xioctl(vfd, VIDIOC_STREAMON, &type, "STREAMON");
	for (int f = 0; f < frames; f++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		struct dma_buf_sync sync = {
			.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
		};
		double t0, t1, t2, tq;

		/* Wait until a frame is done, so DQBUF only does its own work */
		usleep(40000);
		tq = now_ms();
		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF");
		tq = now_ms() - tq;
		/*
		 * DQBUF already invalidated the cache for this buffer. Time a
		 * second invalidation through the dma-buf: it costs the same.
		 */
		t0 = now_ms();
		xioctl(dmafd[b.index], DMA_BUF_IOCTL_SYNC, &sync, "SYNC");
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		xioctl(dmafd[b.index], DMA_BUF_IOCTL_SYNC, &sync, "SYNC");
		t1 = now_ms();
		frontend(vaddr[b.index], fmt.fmt.pix.bytesperline, out, scalar);
		t2 = now_ms();
		if (f >= 5) {	/* skip warm-up */
			t_inv += t1 - t0;
			t_dq += tq;
			t_fe += t2 - t1;
			if (t2 - t1 > t_fe_max)
				t_fe_max = t2 - t1;
		}
		if (prefix && f == frames - 1) {
			dump(prefix, "raw", vaddr[b.index], fmt.fmt.pix.sizeimage);
			dump(prefix, "rgb", out, sizeof(out));
		}
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF");
	}
	xioctl(vfd, VIDIOC_STREAMOFF, &type, "STREAMOFF");

	printf("%s front end, %s buffers: mean %.2f ms, max %.2f ms; DQBUF %.2f ms; dma-buf sync %.2f ms; %d frames\n",
	       scalar ? "scalar" : "NEON", uncached ? "uncached" : "cached",
	       t_fe / (frames - 5), t_fe_max, t_dq / (frames - 5),
	       t_inv / (frames - 5), frames - 5);
	return 0;
}
