/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CROSSOVER_H_
#define CROSSOVER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* An LR4 filter is two biquads with the same parameters connected in series:
 *
 * x -- [BIQUAD] -- y -- [BIQUAD] -- z
 *
 * Both biquad filter has the same parameter b[012] and a[12],
 * The variable [xyz][12] keep the history values.
 */
struct lr4 {
	float b0, b1, b2;
	float a1, a2;
	float x1, x2;
	float y1, y2;
	float z1, z2;
};

/* Three bands crossover filter:
 *
 * INPUT --+-- lp0 --+-- lp1 --+---> LOW (0)
 *         |         |         |
 *         |         \-- hp1 --/
 *         |
 *         \-- hp0 --+-- lp2 ------> MID (1)
 *                   |
 *                   \-- hp2 ------> HIGH (2)
 *
 *            [f0]       [f1]
 *
 * Each lp or hp is an LR4 filter, which consists of two second-order
 * lowpass or highpass butterworth filters.
 */
struct crossover {
	struct lr4 lp[3], hp[3];
};

/* Initializes a crossover filter
 * Args:
 *    xo - The crossover filter we want to initialize.
 *    freq1 - The normalized frequency splits low and mid band.
 *    freq2 - The normalized frequency splits mid and high band.
 */
void crossover_init(struct crossover *xo, float freq1, float freq2);

/* Splits input samples to three bands.
 * Args:
 *    xo - The crossover filter to use.
 *    count - The number of input samples.
 *    data0 - The input samples, also the place to store low band output.
 *    data1 - The place to store mid band output.
 *    data2 - The place to store high band output.
 */
void crossover_process(struct crossover *xo, int count, float *data0,
		       float *data1, float *data2);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOVER_H_ */
