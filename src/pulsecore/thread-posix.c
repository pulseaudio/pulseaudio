/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <sched.h>
#include <errno.h>

#include <pulse/xmalloc.h>
#include <pulsecore/mutex.h>
#include <pulsecore/once.h>
#include <pulsecore/atomic.h>

#include "thread.h"

#define ASSERT_SUCCESS(x) do { \
    int _r = (x); \
    assert(_r == 0); \
} while(0)

struct pa_thread {
    pthread_t id;
    pa_thread_func_t thread_func;
    void *userdata;
    pa_atomic_int_t running;
};

struct pa_tls {
    pthread_key_t key;
};

static pa_tls *thread_tls;
static pa_once_t thread_tls_once = PA_ONCE_INIT;

static void tls_free_cb(void *p) {
    pa_thread *t = p;

    assert(t);

    if (!t->thread_func)
        /* This is a foreign thread, we need to free the struct */
        pa_xfree(t);
}

static void thread_tls_once_func(void) {
    thread_tls = pa_tls_new(tls_free_cb);
    assert(thread_tls);
}

static void* internal_thread_func(void *userdata) {
    pa_thread *t = userdata;
    assert(t);

    t->id = pthread_self();

    pa_once(&thread_tls_once, thread_tls_once_func);

    pa_tls_set(thread_tls, t);

    pa_atomic_inc(&t->running);
    t->thread_func(t->userdata);
    pa_atomic_add(&t->running, -2);

    return NULL;
}

pa_thread* pa_thread_new(pa_thread_func_t thread_func, void *userdata) {
    pa_thread *t;

    assert(thread_func);

    t = pa_xnew(pa_thread, 1);
    t->thread_func = thread_func;
    t->userdata = userdata;
    pa_atomic_store(&t->running, 0);

    if (pthread_create(&t->id, NULL, internal_thread_func, t) < 0) {
        pa_xfree(t);
        return NULL;
    }

    pa_atomic_inc(&t->running);

    return t;
}

int pa_thread_is_running(pa_thread *t) {
    assert(t);

    if (!t->thread_func) {
        /* Mhmm, this is a foreign thread, t->running is not
         * necessarily valid. We misuse pthread_getschedparam() to
         * check if the thread is valid. This might not be portable. */

        int policy;
        struct sched_param param;

        return pthread_getschedparam(t->id, &policy, &param) >= 0 || errno != ESRCH;
    }

    return pa_atomic_load(&t->running) > 0;
}

void pa_thread_free(pa_thread *t) {
    assert(t);

    pa_thread_join(t);
    pa_xfree(t);
}

int pa_thread_join(pa_thread *t) {
    assert(t);

    return pthread_join(t->id, NULL);
}

pa_thread* pa_thread_self(void) {
    pa_thread *t;

    pa_once(&thread_tls_once, thread_tls_once_func);

    if ((t = pa_tls_get(thread_tls)))
        return t;

    /* This is a foreign thread, let's create a pthread structure to
     * make sure that we can always return a sensible pointer */

    t = pa_xnew(pa_thread, 1);
    t->id = pthread_self();
    t->thread_func = NULL;
    t->userdata = NULL;
    pa_atomic_store(&t->running, 2);

    pa_tls_set(thread_tls, t);

    return t;
}

void* pa_thread_get_data(pa_thread *t) {
    assert(t);

    return t->userdata;
}

void pa_thread_set_data(pa_thread *t, void *userdata) {
    assert(t);

    t->userdata = userdata;
}

void pa_thread_yield(void) {
#ifdef HAVE_PTHREAD_YIELD
    pthread_yield();
#else
    ASSERT_SUCCESS(sched_yield());
#endif
}

pa_tls* pa_tls_new(pa_free_cb_t free_cb) {
    pa_tls *t;

    t = pa_xnew(pa_tls, 1);

    if (pthread_key_create(&t->key, free_cb) < 0) {
        pa_xfree(t);
        return NULL;
    }

    return t;
}

void pa_tls_free(pa_tls *t) {
    assert(t);

    ASSERT_SUCCESS(pthread_key_delete(t->key));
    pa_xfree(t);
}

void *pa_tls_get(pa_tls *t) {
    assert(t);

    return pthread_getspecific(t->key);
}

void *pa_tls_set(pa_tls *t, void *userdata) {
    void *r;

    r = pthread_getspecific(t->key);
    ASSERT_SUCCESS(pthread_setspecific(t->key, userdata));
    return r;
}

