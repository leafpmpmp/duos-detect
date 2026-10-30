// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Front end check and benchmark on a captured raw frame: runs the SIMD path
 * (NEON or XTheadVector, when available) and the scalar path, compares the
 * outputs byte for byte, and times both.
 *
 *   fe-bench frame.raw WIDTH HEIGHT OUT_W OUT_H [iterations]
 *
 * The frame is packed RAW12 BGGR as the VI writes it (tools/capture.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frontend.h"

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double run(struct frontend *fe, const uint8_t *raw, int8_t *out, int iter)
{
	double t, best = 1e9;

	for (int i = 0; i < iter; i++) {
		t = now_ms();
		fe_run(fe, raw, out);
		t = now_ms() - t;
		if (t < best)
			best = t;
	}
	return best;
}

int main(int argc, char **argv)
{
	struct fe_params p = {
		.bayer = "BGGR", .black = 16, .white = 255, .gamma = 2.2f,
		.gains = { 1.942f, 1.0f, 1.623f }, .out_scale = 127,
	};
	struct frontend *simd, *scalar;
	int iter = argc > 6 ? atoi(argv[6]) : 20;
	size_t size, n;
	uint8_t *raw;
	int8_t *a, *b;
	FILE *f;

	if (argc < 6) {
		fprintf(stderr, "usage: %s frame.raw WIDTH HEIGHT OUT_W OUT_H [iterations]\n",
			argv[0]);
		return 1;
	}
	p.width = atoi(argv[2]);
	p.height = atoi(argv[3]);
	p.out_w = atoi(argv[4]);
	p.out_h = atoi(argv[5]);
	p.stride = (p.width * 3 / 2 + 15) / 16 * 16;
	/* Same placement as duos-detect: centre, or pad with grey */
	if (p.width / 4 >= p.out_w) {
		p.left = (p.width / 4 - p.out_w) / 2;
	} else {
		p.pad = (p.out_w - p.width / 4 + 1) / 2;
		p.pad_value = 114 / 255.0f;
	}
	p.top = (p.height / 4 - p.out_h) / 2;

	size = (size_t)p.stride * p.height;
	n = (size_t)3 * p.out_w * p.out_h;
	raw = malloc(size);
	a = malloc(n);
	b = malloc(n);
	f = fopen(argv[1], "rb");
	if (!raw || !a || !b || !f || fread(raw, 1, size, f) != size) {
		fprintf(stderr, "%s: need %zu bytes\n", argv[1], size);
		return 1;
	}
	fclose(f);

	simd = fe_create(&p);
	p.no_simd = 1;
	scalar = fe_create(&p);
	if (!simd || !scalar) {
		fprintf(stderr, "bad geometry\n");
		return 1;
	}
	printf("%dx%d -> %dx%d: %s %.2f ms, scalar %.2f ms (best of %d)\n",
	       p.width, p.height, p.out_w, p.out_h, fe_simd(simd),
	       run(simd, raw, a, iter), run(scalar, raw, b, iter), iter);
	if (memcmp(a, b, n)) {
		size_t d = 0;

		for (size_t i = 0; i < n; i++)
			d += a[i] != b[i];
		printf("MISMATCH: %zu of %zu bytes differ\n", d, n);
		return 2;
	}
	printf("outputs identical\n");
	return 0;
}
