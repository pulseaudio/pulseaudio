/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <pulse/sample.h>
#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>

#include "time-smoother.h"

#define HISTORY_MAX 50

/*
 * Implementation of a time smoothing algorithm to synchronize remote
 * clocks to a local one. Evens out noise, adjusts to clock skew and
 * allows cheap estimations of the remote time while clock updates may
 * be seldom and recieved in non-equidistant intervals.
 *
 * Basically, we estimate the gradient of received clock samples in a
 * certain history window (of size 'history_time') with linear
 * regression. With that info we estimate the remote time in
 * 'adjust_time' ahead and smoothen our current estimation function
 * towards that point with a 3rd order polynomial interpolation with
 * fitting derivatives. (more or less a b-spline)
 *
 * The larger 'history_time' is chosen the better we will surpress
 * noise -- but we'll adjust to clock skew slower..
 *
 * The larger 'adjust_time' is chosen the smoother our estimation
 * function will be -- but we'll adjust to clock skew slower, too.
 *
 * If 'monotonic' is TRUE the resulting estimation function is
 * guaranteed to be monotonic.
 */

struct pa_smoother {
    pa_usec_t adjust_time, history_time;
    pa_bool_t monotonic;

    pa_usec_t time_offset;

    pa_usec_t px, py;     /* Point p, where we want to reach stability */
    double dp;            /* Gradient we want at point p */

    pa_usec_t ex, ey;     /* Point e, which we estimated before and need to smooth to */
    double de;            /* Gradient we estimated for point e */

                          /* History of last measurements */
    pa_usec_t history_x[HISTORY_MAX], history_y[HISTORY_MAX];
    unsigned history_idx, n_history;

    /* To even out for monotonicity */
    pa_usec_t last_y;

    /* Cached parameters for our interpolation polynomial y=ax^3+b^2+cx */
    double a, b, c;
    pa_bool_t abc_valid;

    pa_bool_t paused;
    pa_usec_t pause_time;
};

pa_smoother* pa_smoother_new(pa_usec_t adjust_time, pa_usec_t history_time, pa_bool_t monotonic) {
    pa_smoother *s;

    pa_assert(adjust_time > 0);
    pa_assert(history_time > 0);

    s = pa_xnew(pa_smoother, 1);
    s->adjust_time = adjust_time;
    s->history_time = history_time;
    s->time_offset = 0;
    s->monotonic = monotonic;

    s->px = s->py = 0;
    s->dp = 1;

    s->ex = s->ey = 0;
    s->de = 1;

    s->history_idx = 0;
    s->n_history = 0;

    s->last_y = 0;

    s->abc_valid = FALSE;

    s->paused = FALSE;

    return s;
}

void pa_smoother_free(pa_smoother* s) {
    pa_assert(s);

    pa_xfree(s);
}

static void drop_old(pa_smoother *s, pa_usec_t x) {
    unsigned j;

    /* First drop items from history which are too old, but make sure
     * to always keep two entries in the history */

    for (j = s->n_history; j > 2; j--) {

        if (s->history_x[s->history_idx] + s->history_time >= x) {
            /* This item is still valid, and thus all following ones
             * are too, so let's quit this loop */
            break;
        }

        /* Item is too old, let's drop it */
        s->history_idx ++;
        while (s->history_idx >= HISTORY_MAX)
            s->history_idx -= HISTORY_MAX;

        s->n_history --;
    }
}

static void add_to_history(pa_smoother *s, pa_usec_t x, pa_usec_t y) {
    unsigned j;
    pa_assert(s);

    drop_old(s, x);

    /* Calculate position for new entry */
    j = s->history_idx + s->n_history;
    while (j >= HISTORY_MAX)
        j -= HISTORY_MAX;

    /* Fill in entry */
    s->history_x[j] = x;
    s->history_y[j] = y;

    /* Adjust counter */
    s->n_history ++;

    /* And make sure we don't store more entries than fit in */
    if (s->n_history >= HISTORY_MAX) {
        s->history_idx += s->n_history - HISTORY_MAX;
        s->n_history = HISTORY_MAX;
    }
}

static double avg_gradient(pa_smoother *s, pa_usec_t x) {
    unsigned i, j, c = 0;
    int64_t ax = 0, ay = 0, k, t;
    double r;

    drop_old(s, x);

    /* First, calculate average of all measurements */
    i = s->history_idx;
    for (j = s->n_history; j > 0; j--) {

        ax += s->history_x[i];
        ay += s->history_y[i];
        c++;

        i++;
        while (i >= HISTORY_MAX)
            i -= HISTORY_MAX;
    }

    /* Too few measurements, assume gradient of 1 */
    if (c < 2)
        return 1;

    ax /= c;
    ay /= c;

    /* Now, do linear regression */
    k = t = 0;

    i = s->history_idx;
    for (j = s->n_history; j > 0; j--) {
        int64_t dx, dy;

        dx = (int64_t) s->history_x[i] - ax;
        dy = (int64_t) s->history_y[i] - ay;

        k += dx*dy;
        t += dx*dx;

        i++;
        while (i >= HISTORY_MAX)
            i -= HISTORY_MAX;
    }

    r = (double) k / t;

    return s->monotonic && r < 0 ? 0 : r;
}

static void estimate(pa_smoother *s, pa_usec_t x, pa_usec_t *y, double *deriv) {
    pa_assert(s);
    pa_assert(y);

    if (x >= s->px) {
        int64_t t;

        /* The requested point is right of the point where we wanted
         * to be on track again, thus just linearly estimate */

        t = (int64_t) s->py + (int64_t) (s->dp * (x - s->px));

        if (t < 0)
            t = 0;

        *y = (pa_usec_t) t;

        if (deriv)
            *deriv = s->dp;

    } else {

        if (!s->abc_valid) {
            pa_usec_t ex, ey, px, py;
            int64_t kx, ky;
            double de, dp;

            /* Ok, we're not yet on track, thus let's interpolate, and
             * make sure that the first derivative is smooth */

            /* We have two points: (ex|ey) and (px|py) with two gradients
             * at these points de and dp. We do a polynomial interpolation
             * of degree 3 with these 6 values */

            ex = s->ex; ey = s->ey;
            px = s->px; py = s->py;
            de = s->de; dp = s->dp;

            pa_assert(ex < px);

            /* To increase the dynamic range and symplify calculation, we
             * move these values to the origin */
            kx = (int64_t) px - (int64_t) ex;
            ky = (int64_t) py - (int64_t) ey;

            /* Calculate a, b, c for y=ax^3+b^2+cx */
            s->c = de;
            s->b = (((double) (3*ky)/kx - dp - 2*de)) / kx;
            s->a = (dp/kx - 2*s->b - de/kx) / (3*kx);

            s->abc_valid = TRUE;
        }

        /* Move to origin */
        x -= s->ex;

        /* Horner scheme */
        *y = (pa_usec_t) ((double) x * (s->c + (double) x * (s->b + (double) x * s->a)));

        /* Move back from origin */
        *y += s->ey;

        /* Horner scheme */
        if (deriv)
            *deriv = s->c + ((double) x * (s->b*2 + (double) x * s->a*3));
    }

    /* Guarantee monotonicity */
    if (s->monotonic) {

        if (*y < s->last_y)
            *y = s->last_y;
        else
            s->last_y = *y;

        if (deriv && *deriv < 0)
            *deriv = 0;
    }
}

void pa_smoother_put(pa_smoother *s, pa_usec_t x, pa_usec_t y) {
    pa_usec_t ney;
    double nde;

    pa_assert(s);
    pa_assert(x >= s->time_offset);

    /* Fix up x value */
    if (s->paused)
        x = s->pause_time;

    pa_assert(x >= s->time_offset);
    x -= s->time_offset;

    pa_assert(x >= s->ex);

    /* First, we calculate the position we'd estimate for x, so that
     * we can adjust our position smoothly from this one */
    estimate(s, x, &ney, &nde);
    s->ex = x; s->ey = ney; s->de = nde;

    /* Then, we add the new measurement to our history */
    add_to_history(s, x, y);

    /* And determine the average gradient of the history */
    s->dp = avg_gradient(s, x);

    /* And calculate when we want to be on track again */
    s->px = x + s->adjust_time;
    s->py = y + s->dp *s->adjust_time;

    s->abc_valid = FALSE;
}

pa_usec_t pa_smoother_get(pa_smoother *s, pa_usec_t x) {
    pa_usec_t y;

    pa_assert(s);
    pa_assert(x >= s->time_offset);

    /* Fix up x value */
    if (s->paused)
        x = s->pause_time;

    pa_assert(x >= s->time_offset);
    x -= s->time_offset;

    pa_assert(x >= s->ex);

    estimate(s, x, &y, NULL);
    return y;
}

void pa_smoother_set_time_offset(pa_smoother *s, pa_usec_t offset) {
    pa_assert(s);

    s->time_offset = offset;
}

void pa_smoother_pause(pa_smoother *s, pa_usec_t x) {
    pa_assert(s);

    if (s->paused)
        return;

    s->paused = TRUE;
    s->pause_time = x;
}

void pa_smoother_resume(pa_smoother *s, pa_usec_t x) {
    pa_assert(s);

    if (!s->paused)
        return;

    pa_assert(x >= s->pause_time);

    s->paused = FALSE;
    s->time_offset += x - s->pause_time;
}
