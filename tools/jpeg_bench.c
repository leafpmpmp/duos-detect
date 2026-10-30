// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Preview encoder check: planar RGB uint8 [3][h][w] in, JPEG with test boxes
 * out (written to OUT.jpg). Times every encoder route this CPU has and
 * checks that the vector YCbCr conversion matches the scalar one.
 *
 *   jpeg-bench in.rgb W H out.jpg [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "preview.h"
#include "ycc.h"

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	static const char *names[] = { "auto", "RGB", "YCbCr scalar", "YCbCr XTheadVector" };
	struct det_box box[2] = {
		{ 40, 30, 140, 230, 0.9f, 0 },
		{ 300, 60, 380, 300, 0.8f, 0 },
	};
	int w, h, iter = argc > 5 ? atoi(argv[5]) : 20;
	unsigned char *jpg = NULL;
	unsigned long n = 0;
	size_t size;
	uint8_t *u8;
	int8_t *in, *work;
	FILE *f;

	if (argc < 5) {
		fprintf(stderr, "usage: %s in.rgb W H out.jpg [iterations]\n", argv[0]);
		return 1;
	}
	w = atoi(argv[2]);
	h = atoi(argv[3]);
	size = (size_t)3 * w * h;
	u8 = malloc(size);
	in = malloc(size);
	work = malloc(size);
	f = fopen(argv[1], "rb");
	if (!u8 || !in || !work || !f || fread(u8, 1, size, f) != size) {
		fprintf(stderr, "%s: need %zu bytes\n", argv[1], size);
		return 1;
	}
	fclose(f);
	for (size_t i = 0; i < size; i++)	/* as the front end writes it */
		in[i] = (int8_t)((u8[i] * 127 + 127) / 255);

#ifdef __riscv
	if (cpu_xtheadvector()) {
		size_t p = (size_t)w * h, m = p + p / 2;
		uint8_t *a = malloc(m), *b = malloc(m);

		ycc420(in, in + p, in + 2 * p, w, h, a, a + p, a + p + p / 4);
		ycc420_xtv(in, in + p, in + 2 * p, w, h, b, b + p, b + p + p / 4);
		printf("YCbCr XTheadVector vs scalar: %s\n",
		       memcmp(a, b, m) ? "MISMATCH" : "identical");
		free(a);
		free(b);
	}
#endif
	for (int mode = PREVIEW_RGB; mode <= PREVIEW_YCC_XTV; mode++) {
		double best = 1e9;

		if (mode == PREVIEW_YCC_XTV && !cpu_xtheadvector())
			continue;
		for (int i = 0; i < iter; i++) {
			double t;

			memcpy(work, in, size);	/* boxes are drawn into it */
			free(jpg);
			t = now_ms();
			jpg = preview_jpeg(work, w, h, box, 2, 75, mode, &n);
			t = now_ms() - t;
			if (t < best)
				best = t;
		}
		printf("%dx%d %-18s %6lu bytes %6.2f ms (best of %d)\n", w, h,
		       names[mode], n, best, iter);
	}
	f = fopen(argv[4], "wb");	/* the last route's JPEG */
	if (!jpg || !f || fwrite(jpg, 1, n, f) != n) {
		perror(argv[4]);
		return 1;
	}
	fclose(f);
	return 0;
}
