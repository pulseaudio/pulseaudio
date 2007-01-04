/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <assert.h>
#include <pthread.h>

#include <atomic_ops.h>

#include <pulse/xmalloc.h>

#include "mutex.h"

#define ASSERT_SUCCESS(x) do { \
    int _r = (x); \
    assert(_r == 0); \
} while(0)

struct pa_mutex {
    pthread_mutex_t mutex;
};

struct pa_cond {
    pthread_cond_t cond;
};

pa_mutex* pa_mutex_new(int recursive) {
    pa_mutex *m;
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);

    if (recursive)
        ASSERT_SUCCESS(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));

    m = pa_xnew(pa_mutex, 1);

    ASSERT_SUCCESS(pthread_mutex_init(&m->mutex, &attr));
    return m;
}

void pa_mutex_free(pa_mutex *m) {
    assert(m);

    ASSERT_SUCCESS(pthread_mutex_destroy(&m->mutex));
    pa_xfree(m);
}

void pa_mutex_lock(pa_mutex *m) {
    assert(m);

    ASSERT_SUCCESS(pthread_mutex_lock(&m->mutex));
}

void pa_mutex_unlock(pa_mutex *m) {
    assert(m);

    ASSERT_SUCCESS(pthread_mutex_unlock(&m->mutex));
}

pa_cond *pa_cond_new(void) {
    pa_cond *c;

    c = pa_xnew(pa_cond, 1);

    ASSERT_SUCCESS(pthread_cond_init(&c->cond, NULL));
    return c;
}

void pa_cond_free(pa_cond *c) {
    assert(c);

    ASSERT_SUCCESS(pthread_cond_destroy(&c->cond));
    pa_xfree(c);
}

void pa_cond_signal(pa_cond *c, int broadcast) {
    assert(c);

    if (broadcast)
        ASSERT_SUCCESS(pthread_cond_broadcast(&c->cond));
    else
        ASSERT_SUCCESS(pthread_cond_signal(&c->cond));
}

int pa_cond_wait(pa_cond *c, pa_mutex *m) {
    assert(c);
    assert(m);

    return pthread_cond_wait(&c->cond, &m->mutex);
}
