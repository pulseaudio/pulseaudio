/***
  This file is part of PulseAudio.

  Copyright 2009 Kim Lester <kim@dfusion.com.au>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <Multiprocessing.h>

#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>

#include "semaphore.h"

struct pa_semaphore {
    MPSemaphoreID sema;
};

pa_semaphore* pa_semaphore_new(unsigned int value) {
    /* NOTE: Can't assume boolean - ie value = 0,1, so use UINT_MAX (boolean more efficient ?) */
    pa_semaphore *s;

    s = pa_xnew(pa_semaphore, 1);
    pa_assert_se(MPCreateSemaphore(UINT_MAX, value, &s->sema) == 0);

    return s;
}

void pa_semaphore_free(pa_semaphore *s) {
    pa_assert(s);
    pa_assert_se(MPDeleteSemaphore(s->sema) == 0);
    pa_xfree(s);
}

void pa_semaphore_post(pa_semaphore *s) {
    pa_assert(s);
    pa_assert_se(MPSignalSemaphore(s->sema) == 0);
}

void pa_semaphore_wait(pa_semaphore *s) {
    pa_assert(s);
    /* should probably check return value (-ve is error), noErr is ok. */
    pa_assert_se(MPWaitOnSemaphore(s->sema, kDurationForever) == 0);
}
