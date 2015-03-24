/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "crossover.h"
#include "biquad.h"

static void lr4_set(struct lr4 *lr4, enum biquad_type type, float freq)
{
	struct biquad q;
	biquad_set(&q, type, freq, 0, 0);
	lr4->b0 = q.b0;
	lr4->b1 = q.b1;
	lr4->b2 = q.b2;
	lr4->a1 = q.a1;
	lr4->a2 = q.a2;
	lr4->x1 = 0;
	lr4->x2 = 0;
	lr4->y1 = 0;
	lr4->y2 = 0;
	lr4->z1 = 0;
	lr4->z2 = 0;
}

/* Split input data using two LR4 filters, put the result into the input array
 * and another array.
 *
 * data0 --+-- lp --> data0
 *         |
 *         \-- hp --> data1
 */
static void lr4_split(struct lr4 *lp, struct lr4 *hp, int count, float *data0,
		      float *data1)
{
	float lx1 = lp->x1;
	float lx2 = lp->x2;
	float ly1 = lp->y1;
	float ly2 = lp->y2;
	float lz1 = lp->z1;
	float lz2 = lp->z2;
	float lb0 = lp->b0;
	float lb1 = lp->b1;
	float lb2 = lp->b2;
	float la1 = lp->a1;
	float la2 = lp->a2;

	float hx1 = hp->x1;
	float hx2 = hp->x2;
	float hy1 = hp->y1;
	float hy2 = hp->y2;
	float hz1 = hp->z1;
	float hz2 = hp->z2;
	float hb0 = hp->b0;
	float hb1 = hp->b1;
	float hb2 = hp->b2;
	float ha1 = hp->a1;
	float ha2 = hp->a2;

	int i;
	for (i = 0; i < count; i++) {
		float x, y, z;
		x = data0[i];
		y = lb0*x + lb1*lx1 + lb2*lx2 - la1*ly1 - la2*ly2;
		z = lb0*y + lb1*ly1 + lb2*ly2 - la1*lz1 - la2*lz2;
		lx2 = lx1;
		lx1 = x;
		ly2 = ly1;
		ly1 = y;
		lz2 = lz1;
		lz1 = z;
		data0[i] = z;

		y = hb0*x + hb1*hx1 + hb2*hx2 - ha1*hy1 - ha2*hy2;
		z = hb0*y + hb1*hy1 + hb2*hy2 - ha1*hz1 - ha2*hz2;
		hx2 = hx1;
		hx1 = x;
		hy2 = hy1;
		hy1 = y;
		hz2 = hz1;
		hz1 = z;
		data1[i] = z;
	}

	lp->x1 = lx1;
	lp->x2 = lx2;
	lp->y1 = ly1;
	lp->y2 = ly2;
	lp->z1 = lz1;
	lp->z2 = lz2;

	hp->x1 = hx1;
	hp->x2 = hx2;
	hp->y1 = hy1;
	hp->y2 = hy2;
	hp->z1 = hz1;
	hp->z2 = hz2;
}

/* Split input data using two LR4 filters and sum them back to the original
 * data array.
 *
 * data --+-- lp --+--> data
 *        |        |
 *        \-- hp --/
 */
static void lr4_merge(struct lr4 *lp, struct lr4 *hp, int count, float *data)
{
	float lx1 = lp->x1;
	float lx2 = lp->x2;
	float ly1 = lp->y1;
	float ly2 = lp->y2;
	float lz1 = lp->z1;
	float lz2 = lp->z2;
	float lb0 = lp->b0;
	float lb1 = lp->b1;
	float lb2 = lp->b2;
	float la1 = lp->a1;
	float la2 = lp->a2;

	float hx1 = hp->x1;
	float hx2 = hp->x2;
	float hy1 = hp->y1;
	float hy2 = hp->y2;
	float hz1 = hp->z1;
	float hz2 = hp->z2;
	float hb0 = hp->b0;
	float hb1 = hp->b1;
	float hb2 = hp->b2;
	float ha1 = hp->a1;
	float ha2 = hp->a2;

	int i;
	for (i = 0; i < count; i++) {
		float x, y, z;
		x = data[i];
		y = lb0*x + lb1*lx1 + lb2*lx2 - la1*ly1 - la2*ly2;
		z = lb0*y + lb1*ly1 + lb2*ly2 - la1*lz1 - la2*lz2;
		lx2 = lx1;
		lx1 = x;
		ly2 = ly1;
		ly1 = y;
		lz2 = lz1;
		lz1 = z;

		y = hb0*x + hb1*hx1 + hb2*hx2 - ha1*hy1 - ha2*hy2;
		z = hb0*y + hb1*hy1 + hb2*hy2 - ha1*hz1 - ha2*hz2;
		hx2 = hx1;
		hx1 = x;
		hy2 = hy1;
		hy1 = y;
		hz2 = hz1;
		hz1 = z;
		data[i] = z + lz1;
	}

	lp->x1 = lx1;
	lp->x2 = lx2;
	lp->y1 = ly1;
	lp->y2 = ly2;
	lp->z1 = lz1;
	lp->z2 = lz2;

	hp->x1 = hx1;
	hp->x2 = hx2;
	hp->y1 = hy1;
	hp->y2 = hy2;
	hp->z1 = hz1;
	hp->z2 = hz2;
}

void crossover_init(struct crossover *xo, float freq1, float freq2)
{
	int i;
	for (i = 0; i < 3; i++) {
		float f = (i == 0) ? freq1 : freq2;
		lr4_set(&xo->lp[i], BQ_LOWPASS, f);
		lr4_set(&xo->hp[i], BQ_HIGHPASS, f);
	}
}

void crossover_process(struct crossover *xo, int count, float *data0,
		       float *data1, float *data2)
{
	lr4_split(&xo->lp[0], &xo->hp[0], count, data0, data1);
	lr4_merge(&xo->lp[1], &xo->hp[1], count, data0);
	lr4_split(&xo->lp[2], &xo->hp[2], count, data1, data2);
}
