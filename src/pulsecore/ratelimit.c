/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/rtclock.h>

#include <pulsecore/log.h>
#include <pulsecore/mutex.h>

#include "ratelimit.h"

static pa_static_mutex mutex = PA_STATIC_MUTEX_INIT;

/* Modelled after Linux' lib/ratelimit.c by Dave Young
 * <hidave.darkstar@gmail.com>, which is licensed GPLv2. */

pa_bool_t pa_ratelimit_test(pa_ratelimit *r) {
    pa_usec_t now;
    pa_mutex *m;

    now = pa_rtclock_now();

    m = pa_static_mutex_get(&mutex, FALSE, FALSE);
    pa_mutex_lock(m);

    pa_assert(r);
    pa_assert(r->interval > 0);
    pa_assert(r->burst > 0);

    if (r->begin <= 0 ||
        r->begin + r->interval < now) {

        if (r->n_missed > 0)
            pa_log_warn("%u events suppressed", r->n_missed);

        r->begin = now;

        /* Reset counters */
        r->n_printed = 0;
        r->n_missed = 0;
        goto good;
    }

    if (r->n_printed <= r->burst)
        goto good;

    r->n_missed++;
    pa_mutex_unlock(m);
    return FALSE;

good:
    r->n_printed++;
    pa_mutex_unlock(m);
    return TRUE;
}
