#ifndef foopulsertclockhfoo
#define foopulsertclockhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <sys/types.h>
#include <time.h>

#include <pulse/sample.h>

/* Something like pulse/timeval.h but based on CLOCK_MONOTONIC and
 * timespec instead of timeval */

struct timespec *pa_rtclock_get(struct timespec *ts);
pa_usec_t pa_rtclock_age(const struct timespec *tv);
int pa_rtclock_hrtimer(void);

struct timespec *pa_timespec_store(struct timespec *a, pa_usec_t u);
pa_usec_t pa_timespec_load(struct timespec *tv);
struct timespec *pa_timespec_add(struct timespec *tv, pa_usec_t t);
pa_usec_t pa_timespec_diff(const struct timespec *a, const struct timespec *b);
int pa_timespec_cmp(const struct timespec *a, const struct timespec *b);

/* timer with a resolution better than this are considered high-resolution */
#define PA_HRTIMER_THRESHOLD_USEC 10

#endif
