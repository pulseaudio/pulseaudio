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
#include <errno.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <pulse/timeval.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>

#include "core-rtclock.h"

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
    tv->tv_usec = ts.tv_nsec / PA_NSEC_PER_USEC;

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
        return ts.tv_sec == 0 && ts.tv_nsec <= (long) (PA_HRTIMER_THRESHOLD_USEC*PA_NSEC_PER_USEC);
#endif

    pa_assert_se(clock_getres(CLOCK_REALTIME, &ts) == 0);
    return ts.tv_sec == 0 && ts.tv_nsec <= (long) (PA_HRTIMER_THRESHOLD_USEC*PA_NSEC_PER_USEC);

#else /* HAVE_CLOCK_GETTIME */

    return FALSE;

#endif
}

#define TIMER_SLACK_NS (int) ((500 * PA_NSEC_PER_USEC))

void pa_rtclock_hrtimer_enable(void) {
#ifdef PR_SET_TIMERSLACK
    int slack_ns;

    if ((slack_ns = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0)) < 0) {
        pa_log_info("PR_GET_TIMERSLACK/PR_SET_TIMERSLACK not supported.");
        return;
    }

    pa_log_debug("Timer slack is set to %i us.", (int) (slack_ns/PA_NSEC_PER_USEC));

    if (slack_ns > TIMER_SLACK_NS) {
        slack_ns = TIMER_SLACK_NS;

        pa_log_debug("Setting timer slack to %i us.", (int) (slack_ns/PA_NSEC_PER_USEC));

        if (prctl(PR_SET_TIMERSLACK, slack_ns, 0, 0, 0) < 0) {
            pa_log_warn("PR_SET_TIMERSLACK failed: %s", pa_cstrerror(errno));
            return;
        }
    }

#endif
}

struct timeval* pa_rtclock_from_wallclock(struct timeval *tv) {

#ifdef HAVE_CLOCK_GETTIME
    struct timeval wc_now, rt_now;

    pa_gettimeofday(&wc_now);
    pa_rtclock_get(&rt_now);

    pa_assert(tv);

    /* pa_timeval_sub() saturates on underflow! */

    if (pa_timeval_cmp(&wc_now, tv) < 0)
        pa_timeval_add(&rt_now, pa_timeval_diff(tv, &wc_now));
    else
        pa_timeval_sub(&rt_now, pa_timeval_diff(&wc_now, tv));

    *tv = rt_now;
#endif

    return tv;
}

pa_usec_t pa_timespec_load(const struct timespec *ts) {

    if (PA_UNLIKELY(!ts))
        return PA_USEC_INVALID;

    return
        (pa_usec_t) ts->tv_sec * PA_USEC_PER_SEC +
        (pa_usec_t) ts->tv_nsec / PA_NSEC_PER_USEC;
}

struct timespec* pa_timespec_store(struct timespec *ts, pa_usec_t v) {
    pa_assert(ts);

    if (PA_UNLIKELY(v == PA_USEC_INVALID)) {
        ts->tv_sec = PA_INT_TYPE_MAX(time_t);
        ts->tv_nsec = (long) (PA_NSEC_PER_SEC-1);
        return NULL;
    }

    ts->tv_sec = (time_t) (v / PA_USEC_PER_SEC);
    ts->tv_nsec = (long) ((v % PA_USEC_PER_SEC) * PA_NSEC_PER_USEC);

    return ts;
}

static struct timeval* wallclock_from_rtclock(struct timeval *tv) {

#ifdef HAVE_CLOCK_GETTIME
    struct timeval wc_now, rt_now;

    pa_gettimeofday(&wc_now);
    pa_rtclock_get(&rt_now);

    pa_assert(tv);

    /* pa_timeval_sub() saturates on underflow! */

    if (pa_timeval_cmp(&rt_now, tv) < 0)
        pa_timeval_add(&wc_now, pa_timeval_diff(tv, &rt_now));
    else
        pa_timeval_sub(&wc_now, pa_timeval_diff(&rt_now, tv));

    *tv = wc_now;
#endif

    return tv;
}

struct timeval* pa_timeval_rtstore(struct timeval *tv, pa_usec_t v, pa_bool_t rtclock) {
    pa_assert(tv);

    if (v == PA_USEC_INVALID)
        return NULL;

    pa_timeval_store(tv, v);

    if (rtclock)
        tv->tv_usec |= PA_TIMEVAL_RTCLOCK;
    else
        wallclock_from_rtclock(tv);

    return tv;
}
