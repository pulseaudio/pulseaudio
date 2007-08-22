/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stddef.h>
#include <sys/time.h>

#include <pulse/timeval.h>
#include <pulsecore/macro.h>

#include "rtclock.h"

struct timespec *pa_timespec_store(struct timespec *a, pa_usec_t u) {
    pa_assert(a);

    a->tv_sec = u / PA_USEC_PER_SEC;

    u -= (pa_usec_t) a->tv_sec * PA_USEC_PER_SEC;
    
    a->tv_nsec = u * 1000;

    return a;
}

pa_usec_t pa_timespec_load(struct timespec *ts) {
    pa_assert(ts);
    
    return (pa_usec_t) ts->tv_sec * PA_USEC_PER_SEC + (pa_usec_t) (ts->tv_nsec / 1000);
}

pa_usec_t pa_timespec_diff(const struct timespec *a, const struct timespec *b) {
    pa_usec_t r;
    
    pa_assert(a);
    pa_assert(b);

    /* Check which whan is the earlier time and swap the two arguments if required. */
    if (pa_timespec_cmp(a, b) < 0) {
        const struct timespec *c;
        c = a;
        a = b;
        b = c;
    }

    /* Calculate the second difference*/
    r = ((pa_usec_t) a->tv_sec - b->tv_sec) * PA_USEC_PER_SEC;

    /* Calculate the microsecond difference */
    if (a->tv_nsec > b->tv_nsec)
        r += (pa_usec_t) ((a->tv_nsec - b->tv_nsec) / 1000);
    else if (a->tv_nsec < b->tv_nsec)
        r -= (pa_usec_t) ((b->tv_nsec - a->tv_nsec) / 1000);

    return r;
}

int pa_timespec_cmp(const struct timespec *a, const struct timespec *b) {
    pa_assert(a);
    pa_assert(b);

    if (a->tv_sec < b->tv_sec)
        return -1;

    if (a->tv_sec > b->tv_sec)
        return 1;

    if (a->tv_nsec < b->tv_nsec)
        return -1;

    if (a->tv_nsec > b->tv_nsec)
        return 1;

    return 0;
}

struct timespec* pa_timespec_add(struct timespec *ts, pa_usec_t v) {
    unsigned long secs;
    pa_assert(ts);

    secs = (unsigned long) (v/PA_USEC_PER_SEC);
    ts->tv_sec += secs;
    v -= ((pa_usec_t) secs) * PA_USEC_PER_SEC;

    ts->tv_nsec += (long) (v*1000);

    /* Normalize */
    while (ts->tv_nsec >= PA_NSEC_PER_SEC) {
        ts->tv_sec++;
        ts->tv_nsec -= PA_NSEC_PER_SEC;
    }

    return ts;
}

pa_usec_t pa_rtclock_age(const struct timespec *ts) {
    struct timespec now;
    pa_assert(ts);

    return pa_timespec_diff(pa_rtclock_get(&now), ts);
}

struct timespec *pa_rtclock_get(struct timespec *ts) {
    static int no_monotonic = 0;

    /* No locking or atomic ops for no_monotonic here */
    
    pa_assert(ts);

    if (!no_monotonic) {
        if (clock_gettime(CLOCK_MONOTONIC, ts) >= 0)
            return ts;
        
        no_monotonic = 1;
    }

    pa_assert_se(clock_gettime(CLOCK_REALTIME, ts) == 0);
    return ts;
}

int pa_rtclock_hrtimer(void) {
    struct timespec ts;
    
    if (clock_getres(CLOCK_MONOTONIC, &ts) >= 0)
        return ts.tv_sec == 0 && ts.tv_nsec <= PA_HRTIMER_THRESHOLD_USEC*1000;

    pa_assert_se(clock_getres(CLOCK_REALTIME, &ts) == 0);
    return ts.tv_sec == 0 && ts.tv_nsec <= PA_HRTIMER_THRESHOLD_USEC*1000;
}

