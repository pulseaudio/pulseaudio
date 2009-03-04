/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>

#include "once.h"

pa_bool_t pa_once_begin(pa_once *control) {
    pa_mutex *m;

    pa_assert(control);

    if (pa_atomic_load(&control->done))
        return FALSE;

    pa_atomic_inc(&control->ref);

    /* Caveat: We have to make sure that the once func has completed
     * before returning, even if the once func is not actually
     * executed by us. Hence the awkward locking. */

    for (;;) {

        if ((m = pa_atomic_ptr_load(&control->mutex))) {

            /* The mutex is stored in locked state, hence let's just
             * wait until it is unlocked */
            pa_mutex_lock(m);

            pa_assert(pa_atomic_load(&control->done));

            pa_once_end(control);
            return FALSE;
        }

        pa_assert_se(m = pa_mutex_new(FALSE, FALSE));
        pa_mutex_lock(m);

        if (pa_atomic_ptr_cmpxchg(&control->mutex, NULL, m))
            return TRUE;

        pa_mutex_unlock(m);
        pa_mutex_free(m);
    }
}

void pa_once_end(pa_once *control) {
    pa_mutex *m;

    pa_assert(control);

    pa_atomic_store(&control->done, 1);

    pa_assert_se(m = pa_atomic_ptr_load(&control->mutex));
    pa_mutex_unlock(m);

    if (pa_atomic_dec(&control->ref) <= 1) {
        pa_assert_se(pa_atomic_ptr_cmpxchg(&control->mutex, m, NULL));
        pa_mutex_free(m);
    }
}

/* Not reentrant -- how could it be? */
void pa_run_once(pa_once *control, pa_once_func_t func) {
    pa_assert(control);
    pa_assert(func);

    if (pa_once_begin(control)) {
        func();
        pa_once_end(control);
    }
}
