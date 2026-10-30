// SPDX-License-Identifier: AGPL-3.0-or-later
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cviruntime.h>

#include "detector.h"

#define MAX_SLOTS	4
#define MAX_LEVELS	4

struct level {
	int box, cls;		/* output tensor index */
	int w, h, stride;
};

struct slot {
	CVI_MODEL_HANDLE model;
	CVI_TENSOR *in, *out;
	int32_t nin, nout;
};

struct detector {
	struct slot slot[MAX_SLOTS];
	int nslots, nlevels, nc, reg;	/* reg: 16 (DFL) or 1 */
	int in_w, in_h;
	struct level lv[MAX_LEVELS];
	struct det_box *cand;	/* one per output cell */
	int ncells;
};

static int tensor_c(CVI_TENSOR *t)
{
	return CVI_NN_TensorShape(t).dim[1];
}

static int setup_levels(struct detector *d)
{
	struct slot *s = &d->slot[0];

	for (int i = 0; i < s->nout; i++) {
		CVI_SHAPE sh = CVI_NN_TensorShape(&s->out[i]);
		int h = sh.dim[2], w = sh.dim[3], l;

		if (s->out[i].fmt != CVI_FMT_INT8) {
			fprintf(stderr, "output %s is not int8\n", s->out[i].name);
			return -1;
		}
		for (l = 0; l < d->nlevels; l++)
			if (d->lv[l].w == w && d->lv[l].h == h)
				break;
		if (l == d->nlevels) {
			if (l == MAX_LEVELS)
				return -1;
			d->nlevels++;
			d->lv[l] = (struct level){ -1, -1, w, h, d->in_h / h };
		}
		/* Ultralytics names: cv2 = box (4 x reg channels), cv3 = class */
		if (strstr(s->out[i].name, "/cv2."))
			d->lv[l].box = i;
		else if (strstr(s->out[i].name, "/cv3."))
			d->lv[l].cls = i;
		else
			return -1;
	}
	for (int l = 0; l < d->nlevels; l++) {
		if (d->lv[l].box < 0 || d->lv[l].cls < 0)
			return -1;
		d->nc = tensor_c(&s->out[d->lv[l].cls]);
		d->reg = tensor_c(&s->out[d->lv[l].box]) / 4;
	}
	return d->nlevels && (d->reg == 16 || d->reg == 1) ? 0 : -1;
}

struct detector *det_open(const char *model, int nslots)
{
	struct detector *d = calloc(1, sizeof(*d));
	CVI_SHAPE sh;

	if (!d || nslots < 1 || nslots > MAX_SLOTS)
		goto err;
	if (CVI_NN_RegisterModel(model, &d->slot[0].model) != CVI_RC_SUCCESS) {
		fprintf(stderr, "cannot load %s\n", model);
		goto err;
	}
	for (int i = 1; i < nslots; i++)
		if (CVI_NN_CloneModel(d->slot[0].model, &d->slot[i].model) !=
		    CVI_RC_SUCCESS)
			goto err;
	d->nslots = nslots;
	for (int i = 0; i < nslots; i++) {
		struct slot *s = &d->slot[i];

		if (CVI_NN_GetInputOutputTensors(s->model, &s->in, &s->nin,
						 &s->out, &s->nout) != CVI_RC_SUCCESS ||
		    s->nin != 1 || s->in[0].fmt != CVI_FMT_INT8) {
			fprintf(stderr, "need one int8 input\n");
			goto err;
		}
	}
	sh = CVI_NN_TensorShape(&d->slot[0].in[0]);
	d->in_h = sh.dim[2];
	d->in_w = sh.dim[3];
	if (sh.dim[1] != 3 || setup_levels(d)) {
		fprintf(stderr, "%s: not a supported YOLO head\n", model);
		goto err;
	}
	for (int l = 0; l < d->nlevels; l++)
		d->ncells += d->lv[l].w * d->lv[l].h;
	d->cand = malloc(d->ncells * sizeof(*d->cand));
	if (!d->cand)
		goto err;
	return d;
err:
	det_close(d);
	return NULL;
}

void det_close(struct detector *d)
{
	if (!d)
		return;
	/* Clones first: they share the weights of slot 0 */
	for (int i = d->nslots - 1; i >= 0; i--)
		if (d->slot[i].model)
			CVI_NN_CleanupModel(d->slot[i].model);
	free(d->cand);
	free(d);
}

void det_input_size(const struct detector *d, int *w, int *h)
{
	*w = d->in_w;
	*h = d->in_h;
}

float det_input_scale(const struct detector *d)
{
	return CVI_NN_TensorQuantScale((CVI_TENSOR *)&d->slot[0].in[0]);
}

int det_classes(const struct detector *d)
{
	return d->nc;
}

int8_t *det_input(struct detector *d, int slot)
{
	return CVI_NN_TensorPtr(&d->slot[slot].in[0]);
}

int det_run(struct detector *d, int slot)
{
	struct slot *s = &d->slot[slot];

	return CVI_NN_Forward(s->model, s->in, s->nin, s->out, s->nout) ==
	       CVI_RC_SUCCESS ? 0 : -1;
}

static float iou(const struct det_box *a, const struct det_box *b)
{
	float w = fminf(a->x2, b->x2) - fmaxf(a->x1, b->x1);
	float h = fminf(a->y2, b->y2) - fmaxf(a->y1, b->y1);
	float i, u;

	if (w <= 0 || h <= 0)
		return 0;
	i = w * h;
	u = (a->x2 - a->x1) * (a->y2 - a->y1) +
	    (b->x2 - b->x1) * (b->y2 - b->y1) - i;
	return i / u;
}

static int by_score(const void *a, const void *b)
{
	float d = ((const struct det_box *)b)->score -
		  ((const struct det_box *)a)->score;

	return (d > 0) - (d < 0);
}

/* Distance (in stride units) from a 16-bin DFL distribution */
static float dfl(const int8_t *p, int step, float qs)
{
	float e[16], m = -1e30f, sum = 0, acc = 0;

	for (int j = 0; j < 16; j++) {
		e[j] = p[j * step] * qs;
		m = fmaxf(m, e[j]);
	}
	for (int j = 0; j < 16; j++) {
		e[j] = expf(e[j] - m);
		sum += e[j];
		acc += j * e[j];
	}
	return acc / sum;
}

int det_decode(struct detector *d, int slot, int cls, float conf,
	       float iou_th, struct det_box *out, int max)
{
	struct slot *s = &d->slot[slot];
	float logit_th = logf(conf / (1 - conf));
	int n = 0, kept = 0;

	for (int l = 0; l < d->nlevels; l++) {
		const struct level *lv = &d->lv[l];
		CVI_TENSOR *bt = &s->out[lv->box], *ct = &s->out[lv->cls];
		const int8_t *b = CVI_NN_TensorPtr(bt);
		const int8_t *c = (const int8_t *)CVI_NN_TensorPtr(ct) +
				  (size_t)cls * lv->w * lv->h;
		float qb = CVI_NN_TensorQuantScale(bt);
		float qc = CVI_NN_TensorQuantScale(ct);
		/* Compare the int8 logits with the threshold: no float per cell */
		int th = (int)ceilf(logit_th / qc);
		int plane = lv->w * lv->h;

		if (th > 127)
			continue;
		for (int i = 0; i < plane; i++) {
			float cx, cy, dist[4];
			struct det_box *o;

			if (c[i] < th)
				continue;
			for (int k = 0; k < 4; k++)
				dist[k] = d->reg == 16 ?
					dfl(b + (size_t)k * 16 * plane + i, plane, qb) :
					b[(size_t)k * plane + i] * qb;
			cx = i % lv->w + 0.5f;
			cy = i / lv->w + 0.5f;
			o = &d->cand[n++];
			o->x1 = (cx - dist[0]) * lv->stride;
			o->y1 = (cy - dist[1]) * lv->stride;
			o->x2 = (cx + dist[2]) * lv->stride;
			o->y2 = (cy + dist[3]) * lv->stride;
			o->score = 1 / (1 + expf(-c[i] * qc));
			o->cls = cls;
		}
	}

	qsort(d->cand, n, sizeof(d->cand[0]), by_score);
	for (int i = 0; i < n && kept < max; i++) {
		int keep = 1;

		for (int j = 0; j < kept && keep; j++)
			keep = iou(&d->cand[i], &out[j]) <= iou_th;
		if (keep)
			out[kept++] = d->cand[i];
	}
	return kept;
}
