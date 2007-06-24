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

#define ASYNCQ_SIZE 128

/* For debugging purposes we can define _Y to put and extra thread
 * yield between each operation. */

#ifdef PROFILE
#define _Y pa_thread_yield()
#else
#define _Y do { } while(0)
#endif

struct pa_asyncq {
    unsigned size;
    unsigned read_idx;
    unsigned write_idx;
    pa_atomic_t read_waiting; /* a bool */
    pa_atomic_t write_waiting; /* a bool */
    int read_fds[2], write_fds[2];
    pa_atomic_t in_read_fifo, in_write_fifo;
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
    pa_atomic_store(&l->read_waiting, 0);
    pa_atomic_store(&l->write_waiting, 0);
    pa_atomic_store(&l->in_read_fifo, 0);
    pa_atomic_store(&l->in_write_fifo, 0);

    if (pipe(l->read_fds) < 0) {
        pa_xfree(l);
        return NULL;
    }

    if (pipe(l->write_fds) < 0) {
        pa_close(l->read_fds[0]);
        pa_close(l->read_fds[1]);
        pa_xfree(l);
        return NULL;
    }

    pa_make_nonblock_fd(l->read_fds[1]);
    pa_make_nonblock_fd(l->write_fds[1]);

    return l;
}

void pa_asyncq_free(pa_asyncq *l, pa_free_cb_t free_cb) {
    pa_assert(l);

    if (free_cb) {
        void *p;

        while ((p = pa_asyncq_pop(l, 0)))
            free_cb(p);
    }

    pa_close(l->read_fds[0]);
    pa_close(l->read_fds[1]);
    pa_close(l->write_fds[0]);
    pa_close(l->write_fds[1]);

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

        /* Let's empty the FIFO from old notifications, before we return */
            
        while (pa_atomic_load(&l->in_write_fifo) > 0) {
            ssize_t r;
            int x[20];

            if ((r = read(l->write_fds[0], x, sizeof(x))) < 0) {

                if (errno == EINTR)
                    continue;
                
                return -1;
            }

            pa_assert(r > 0);
                
            if (pa_atomic_sub(&l->in_write_fifo, r) <= r)
                break;

        }

        /* Now let's make sure that we didn't lose any events */
        if (!pa_atomic_ptr_cmpxchg(&cells[idx], NULL, p)) {

            if (!wait)
                return -1;

            /* Let's wait for changes. */

            _Y;

            pa_assert_se(pa_atomic_cmpxchg(&l->write_waiting, 0, 1));

            for (;;) {
                char x[20];
                ssize_t r;
                
                _Y;
                
                if (pa_atomic_ptr_cmpxchg(&cells[idx], NULL, p))
                    break;
                
                _Y;

                if ((r = read(l->write_fds[0], x, sizeof(x))) < 0) {

                    if (errno == EINTR)
                        continue;
                    
                    pa_assert_se(pa_atomic_cmpxchg(&l->write_waiting, 1, 0));
                    return -1;
                }

                pa_assert(r > 0);
                pa_atomic_sub(&l->in_write_fifo, r);
            }
            
            _Y;
            
            pa_assert_se(pa_atomic_cmpxchg(&l->write_waiting, 1, 0));
        }
    }

    _Y;
    l->write_idx++;

    if (pa_atomic_load(&l->read_waiting) > 0) {
        char x = 'x';
        _Y;
        if (write(l->read_fds[1], &x, sizeof(x)) > 0) {
            pa_atomic_inc(&l->in_read_fifo);
/*             pa_log("increasing %p by 1", l); */
        }
    }

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

/*         pa_log("pop failed wait=%i", wait); */

        /* Hmm, nothing, here, so let's drop all queued events. */
        while (pa_atomic_load(&l->in_read_fifo) > 0) {
            ssize_t r;
            int x[20];
            
            if ((r = read(l->read_fds[0], x, sizeof(x))) < 0) {
                
                if (errno == EINTR)
                    continue;
                
                return NULL;
            }

            pa_assert(r > 0);

/*             pa_log("decreasing %p by %i", l, r); */
            
            if (pa_atomic_sub(&l->in_read_fifo, r) <= r)
                break;
        }

        /* Now let's make sure that we didn't lose any events */
        if (!(ret = pa_atomic_ptr_load(&cells[idx]))) {

            if (!wait)
                return NULL;

            /* Let's wait for changes. */
            
            _Y;
            
            pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 0, 1));
            
            for (;;) {
                char x[20];
                ssize_t r;
                
                _Y;
                
                if ((ret = pa_atomic_ptr_load(&cells[idx])))
                    break;
                
                _Y;
                
                if ((r = read(l->read_fds[0], x, sizeof(x))) < 0) {

                    if (errno == EINTR)
                        continue;
                    
                    pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 1, 0));
                    return NULL;
                }

/*                 pa_log("decreasing %p by %i", l, r); */
                
                pa_assert(r > 0);
                pa_atomic_sub(&l->in_read_fifo, r);
            }

            _Y;

            pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 1, 0));
        }
    }

    pa_assert(ret);

    /* Guaranteed if we only have a single reader */
    pa_assert_se(pa_atomic_ptr_cmpxchg(&cells[idx], ret, NULL));

    _Y;
    l->read_idx++;

    if (pa_atomic_load(&l->write_waiting) > 0) {
        char x = 'x';
        _Y;
        if (write(l->write_fds[1], &x, sizeof(x)) >= 0)
            pa_atomic_inc(&l->in_write_fifo);
    }

    return ret;
}

int pa_asyncq_get_fd(pa_asyncq *q) {
    pa_assert(q);

    return q->read_fds[0];
}

int pa_asyncq_before_poll(pa_asyncq *l) {
    int idx;
    pa_atomic_ptr_t *cells;

    pa_assert(l);

    cells = PA_ASYNCQ_CELLS(l);

    _Y;
    idx = reduce(l, l->read_idx);

    if (pa_atomic_ptr_load(&cells[idx]) || pa_atomic_load(&l->in_read_fifo) > 0)
        return -1;

    pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 0, 1));

    if (pa_atomic_ptr_load(&cells[idx]) || pa_atomic_load(&l->in_read_fifo) > 0) {
        pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 1, 0));
        return -1;
    }

    return 0;
}

void pa_asyncq_after_poll(pa_asyncq *l) {
    pa_assert(l);

    pa_assert_se(pa_atomic_cmpxchg(&l->read_waiting, 1, 0));
}
