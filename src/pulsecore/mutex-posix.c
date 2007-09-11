/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <pthread.h>

#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>

#include "mutex.h"

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
        pa_assert_se(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);

    m = pa_xnew(pa_mutex, 1);
    pa_assert_se(pthread_mutex_init(&m->mutex, &attr) == 0);
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
