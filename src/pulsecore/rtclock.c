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
#include <time.h>
#include <sys/time.h>

#include <pulse/timeval.h>
#include <pulsecore/macro.h>

#include "rtclock.h"

pa_usec_t pa_rtclock_age(const struct timeval *tv) {
    struct timeval now;
    pa_assert(tv);

    return pa_timeval_diff(pa_rtclock_get(&now), tv);
}

struct timeval *pa_rtclock_get(struct timeval *tv) {
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;

#ifdef CLOCK_MONOTONIC
    /* No locking or atomic ops for no_monotonic here */
    static pa_bool_t no_monotonic = FALSE;

    if (!no_monotonic)
        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
            no_monotonic = TRUE;

    if (no_monotonic)
#endif
        pa_assert_se(clock_gettime(CLOCK_REALTIME, &ts) == 0);

    pa_assert(tv);

    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;

    return tv;

#else /* HAVE_CLOCK_GETTIME */

    return pa_gettimeofday(tv);

#endif
}

pa_bool_t pa_rtclock_hrtimer(void) {
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;

#ifdef CLOCK_MONOTONIC
    if (clock_getres(CLOCK_MONOTONIC, &ts) >= 0)
        return ts.tv_sec == 0 && ts.tv_nsec <= PA_HRTIMER_THRESHOLD_USEC*1000;
#endif

    pa_assert_se(clock_getres(CLOCK_REALTIME, &ts) == 0);
    return ts.tv_sec == 0 && ts.tv_nsec <= PA_HRTIMER_THRESHOLD_USEC*1000;

#else /* HAVE_CLOCK_GETTIME */

    return FALSE;

#endif
}

pa_usec_t pa_rtclock_usec(void) {
    struct timeval tv;

    return pa_timeval_load(pa_rtclock_get(&tv));
}

struct timeval* pa_rtclock_from_wallclock(struct timeval *tv) {

#ifdef HAVE_CLOCK_GETTIME
    struct timeval wc_now, rt_now;

    pa_gettimeofday(&wc_now);
    pa_rtclock_get(&rt_now);

    pa_assert(tv);

    if (pa_timeval_cmp(&wc_now, tv) < 0)
        pa_timeval_add(&rt_now, pa_timeval_diff(tv, &wc_now));
    else
        pa_timeval_sub(&rt_now, pa_timeval_diff(&wc_now, tv));

    *tv = rt_now;
#endif

    return tv;
}
