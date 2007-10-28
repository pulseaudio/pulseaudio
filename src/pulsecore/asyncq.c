/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <unistd.h>
#include <errno.h>

#include <pulsecore/atomic.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulse/xmalloc.h>

#include "asyncq.h"
#include "fdsem.h"

#define ASYNCQ_SIZE 128

/* For debugging purposes we can define _Y to put and extra thread
 * yield between each operation. */

/* #define PROFILE */

#ifdef PROFILE
#define _Y pa_thread_yield()
#else
#define _Y do { } while(0)
#endif

struct pa_asyncq {
    unsigned size;
    unsigned read_idx;
    unsigned write_idx;
    pa_fdsem *read_fdsem, *write_fdsem;
};

#define PA_ASYNCQ_CELLS(x) ((pa_atomic_ptr_t*) ((uint8_t*) (x) + PA_ALIGN(sizeof(struct pa_asyncq))))

static int is_power_of_two(unsigned size) {
    return !(size & (size - 1));
}

static int reduce(pa_asyncq *l, int value) {
    return value & (unsigned) (l->size - 1);
}

pa_asyncq *pa_asyncq_new(unsigned size) {
    pa_asyncq *l;

    if (!size)
        size = ASYNCQ_SIZE;

    pa_assert(is_power_of_two(size));

    l = pa_xmalloc0(PA_ALIGN(sizeof(pa_asyncq)) + (sizeof(pa_atomic_ptr_t) * size));

    l->size = size;

    if (!(l->read_fdsem = pa_fdsem_new())) {
        pa_xfree(l);
        return NULL;
    }

    if (!(l->write_fdsem = pa_fdsem_new())) {
        pa_fdsem_free(l->read_fdsem);
        pa_xfree(l);
        return NULL;
    }

    return l;
}

void pa_asyncq_free(pa_asyncq *l, pa_free_cb_t free_cb) {
    pa_assert(l);

    if (free_cb) {
        void *p;

        while ((p = pa_asyncq_pop(l, 0)))
            free_cb(p);
    }

    pa_fdsem_free(l->read_fdsem);
    pa_fdsem_free(l->write_fdsem);
    pa_xfree(l);
}

int pa_asyncq_push(pa_asyncq*l, void *p, int wait) {
    int idx;
    pa_atomic_ptr_t *cells;

    pa_assert(l);
    pa_assert(p);

    cells = PA_ASYNCQ_CELLS(l);

    _Y;
    idx = reduce(l, l->write_idx);

    if (!pa_atomic_ptr_cmpxchg(&cells[idx], NULL, p)) {

        if (!wait)
            return -1;

/*         pa_log("sleeping on push"); */

        do {
            pa_fdsem_wait(l->read_fdsem);
        } while (!pa_atomic_ptr_cmpxchg(&cells[idx], NULL, p));
    }

    _Y;
    l->write_idx++;

    pa_fdsem_post(l->write_fdsem);

    return 0;
}

void* pa_asyncq_pop(pa_asyncq*l, int wait) {
    int idx;
    void *ret;
    pa_atomic_ptr_t *cells;

    pa_assert(l);

    cells = PA_ASYNCQ_CELLS(l);

    _Y;
    idx = reduce(l, l->read_idx);

    if (!(ret = pa_atomic_ptr_load(&cells[idx]))) {

        if (!wait)
            return NULL;

/*         pa_log("sleeping on pop"); */

        do {
            pa_fdsem_wait(l->write_fdsem);
        } while (!(ret = pa_atomic_ptr_load(&cells[idx])));
    }

    pa_assert(ret);

    /* Guaranteed to succeed if we only have a single reader */
    pa_assert_se(pa_atomic_ptr_cmpxchg(&cells[idx], ret, NULL));

    _Y;
    l->read_idx++;

    pa_fdsem_post(l->read_fdsem);

    return ret;
}

int pa_asyncq_get_fd(pa_asyncq *q) {
    pa_assert(q);

    return pa_fdsem_get(q->write_fdsem);
}

int pa_asyncq_before_poll(pa_asyncq *l) {
    int idx;
    pa_atomic_ptr_t *cells;

    pa_assert(l);

    cells = PA_ASYNCQ_CELLS(l);

    _Y;
    idx = reduce(l, l->read_idx);

    for (;;) {
        if (pa_atomic_ptr_load(&cells[idx]))
            return -1;

        if (pa_fdsem_before_poll(l->write_fdsem) >= 0)
            return 0;
    }

    return 0;
}

void pa_asyncq_after_poll(pa_asyncq *l) {
    pa_assert(l);

    pa_fdsem_after_poll(l->write_fdsem);
}
