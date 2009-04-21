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

#include <pthread.h>
#include <errno.h>

#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>
#include <pulsecore/core-error.h>

#include "mutex.h"

struct pa_mutex {
    pthread_mutex_t mutex;
};

struct pa_cond {
    pthread_cond_t cond;
};

pa_mutex* pa_mutex_new(pa_bool_t recursive, pa_bool_t inherit_priority) {
    pa_mutex *m;
    pthread_mutexattr_t attr;
    int r;

    pa_assert_se(pthread_mutexattr_init(&attr) == 0);

    if (recursive)
        pa_assert_se(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);

#ifdef HAVE_PTHREAD_PRIO_INHERIT
    if (inherit_priority)
        pa_assert_se(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0);
#endif

    m = pa_xnew(pa_mutex, 1);

#ifndef HAVE_PTHREAD_PRIO_INHERIT
    pa_assert_se(pthread_mutex_init(&m->mutex, &attr) == 0);

#else
    if ((r = pthread_mutex_init(&m->mutex, &attr))) {

        /* If this failed, then this was probably due to non-available
         * priority inheritance. In which case we fall back to normal
         * mutexes. */
        pa_assert(r == ENOTSUP && inherit_priority);

        pa_assert_se(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE) == 0);
        pa_assert_se(pthread_mutex_init(&m->mutex, &attr) == 0);
    }
#endif

    return m;
}

void pa_mutex_free(pa_mutex *m) {
    pa_assert(m);

    pa_assert_se(pthread_mutex_destroy(&m->mutex) == 0);
    pa_xfree(m);
}

void pa_mutex_lock(pa_mutex *m) {
    pa_assert(m);

    pa_assert_se(pthread_mutex_lock(&m->mutex) == 0);
}

pa_bool_t pa_mutex_try_lock(pa_mutex *m) {
    int r;
    pa_assert(m);

    if ((r = pthread_mutex_trylock(&m->mutex)) != 0) {
        pa_assert(r == EBUSY);
        return FALSE;
    }

    return TRUE;
}

void pa_mutex_unlock(pa_mutex *m) {
    pa_assert(m);

    pa_assert_se(pthread_mutex_unlock(&m->mutex) == 0);
}

pa_cond *pa_cond_new(void) {
    pa_cond *c;

    c = pa_xnew(pa_cond, 1);
    pa_assert_se(pthread_cond_init(&c->cond, NULL) == 0);
    return c;
}

void pa_cond_free(pa_cond *c) {
    pa_assert(c);

    pa_assert_se(pthread_cond_destroy(&c->cond) == 0);
    pa_xfree(c);
}

void pa_cond_signal(pa_cond *c, int broadcast) {
    pa_assert(c);

    if (broadcast)
        pa_assert_se(pthread_cond_broadcast(&c->cond) == 0);
    else
        pa_assert_se(pthread_cond_signal(&c->cond) == 0);
}

int pa_cond_wait(pa_cond *c, pa_mutex *m) {
    pa_assert(c);
    pa_assert(m);

    return pthread_cond_wait(&c->cond, &m->mutex);
}

pa_mutex* pa_static_mutex_get(pa_static_mutex *s, pa_bool_t recursive, pa_bool_t inherit_priority) {
    pa_mutex *m;

    pa_assert(s);

    /* First, check if already initialized and short cut */
    if ((m = pa_atomic_ptr_load(&s->ptr)))
        return m;

    /* OK, not initialized, so let's allocate, and fill in */
    m = pa_mutex_new(recursive, inherit_priority);
    if ((pa_atomic_ptr_cmpxchg(&s->ptr, NULL, m)))
        return m;

    pa_mutex_free(m);

    /* Him, filling in failed, so someone else must have filled in
     * already */
    pa_assert_se(m = pa_atomic_ptr_load(&s->ptr));
    return m;
}
