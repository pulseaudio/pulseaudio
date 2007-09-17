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

#include <pulse/xmalloc.h>

#include <pulsecore/atomic.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "flist.h"

/* Algorithm is not perfect, in a few corner cases it will fail to pop
 * from the flist although it isn't empty, and fail to push into the
 * flist, although it isn't full.
 *
 * We keep a fixed size array of entries, each item is either marked
 * UNUSED, USED or BUSY and contains a user data pointer. When pushing
 * into the queue we look for an UNUSED cell and mark it BUSY with a
 * CAS operation. If successful we use it and mark it USED, otherwise
 * we go on and look for the next UNUSED cell. The algorithm for
 * popping an item from the queue is practically inverse: look for a
 * USED cell and and mark it BUSY with a CAS operation, after reading
 * from it mark it UNUSED again.
 *
 * To accelerate finding of used/unused cells we maintain a read and a
 * write index which is used like a ring buffer. After each push we
 * increase the write index and after each pop we increase the read
 * index.
 *
 * The indexes are incremented atomically and are never truncated to
 * the buffer size. Instead we assume that the buffer size is a power
 * of two and that the truncation can thus be done by applying a
 * simple AND on read.
 *
 * To make sure that we do not look for empty cells indefinitely we
 * maintain a length value which stores the number of used cells. From
 * this value the number of unused cells is easily calculated. Please
 * note that the length value is not updated atomically with the read
 * and write index and might thus be a few cells off the real
 * value. To deal with this we always look for N_EXTRA_SCAN extra
 * cells when pushing/popping entries.
 *
 * It might make sense to replace this implementation with a link list
 * stack or queue, which however requires DCAS to be simple. Patches
 * welcome.
 *
 * Please note that this algorithm is home grown.*/

#define FLIST_SIZE 128
#define N_EXTRA_SCAN 2

/* For debugging purposes we can define _Y to put and extra thread
 * yield between each operation. */

#ifdef PROFILE
#define _Y pa_thread_yield()
#else
#define _Y do { } while(0)
#endif

enum {
    STATE_UNUSED,
    STATE_USED,
    STATE_BUSY
};

struct cell {
    pa_atomic_t state;
    void *data;
};

struct pa_flist {
    unsigned size;
    pa_atomic_t length;
    pa_atomic_t read_idx;
    pa_atomic_t write_idx;
};

#define PA_FLIST_CELLS(x) ((struct cell*) ((uint8_t*) (x) + PA_ALIGN(sizeof(struct pa_flist))))

pa_flist *pa_flist_new(unsigned size) {
    pa_flist *l;

    if (!size)
        size = FLIST_SIZE;

    pa_assert(pa_is_power_of_two(size));

    l = pa_xmalloc0(PA_ALIGN(sizeof(pa_flist)) + (sizeof(struct cell) * size));

    l->size = size;

    pa_atomic_store(&l->read_idx, 0);
    pa_atomic_store(&l->write_idx, 0);
    pa_atomic_store(&l->length, 0);

    return l;
}

static int reduce(pa_flist *l, int value) {
    return value & (unsigned) (l->size - 1);
}

void pa_flist_free(pa_flist *l, pa_free_cb_t free_cb) {
    pa_assert(l);

    if (free_cb) {
        struct cell *cells;
        int len, idx;

        cells = PA_FLIST_CELLS(l);

        idx = reduce(l, pa_atomic_load(&l->read_idx));
        len = pa_atomic_load(&l->length);

        for (; len > 0; len--) {

            if (pa_atomic_load(&cells[idx].state) == STATE_USED)
                free_cb(cells[idx].data);

            idx = reduce(l, idx + 1);
        }
    }

    pa_xfree(l);
}

int pa_flist_push(pa_flist*l, void *p) {
    int idx, len, n;
    struct cell *cells;

    pa_assert(l);
    pa_assert(p);

    cells = PA_FLIST_CELLS(l);

    n = len = (int) l->size - pa_atomic_load(&l->length) + N_EXTRA_SCAN;
    _Y;
    idx = reduce(l, pa_atomic_load(&l->write_idx));

    for (; n > 0 ; n--) {
        _Y;

        if (pa_atomic_cmpxchg(&cells[idx].state, STATE_UNUSED, STATE_BUSY)) {
            _Y;
            pa_atomic_inc(&l->write_idx);
            _Y;
            cells[idx].data = p;
            _Y;
            pa_atomic_store(&cells[idx].state, STATE_USED);
            _Y;
            pa_atomic_inc(&l->length);
            return 0;
        }

        _Y;
        idx = reduce(l, idx + 1);
    }

#ifdef PROFILE
    if (len > N_EXTRA_SCAN)
        pa_log_warn("Didn't  find free cell after %u iterations.", len);
#endif

    return -1;
}

void* pa_flist_pop(pa_flist*l) {
    int idx, len, n;
    struct cell *cells;

    pa_assert(l);

    cells = PA_FLIST_CELLS(l);

    n = len = pa_atomic_load(&l->length) + N_EXTRA_SCAN;
    _Y;
    idx = reduce(l, pa_atomic_load(&l->read_idx));

    for (; n > 0 ; n--) {
        _Y;

        if (pa_atomic_cmpxchg(&cells[idx].state, STATE_USED, STATE_BUSY)) {
            void *p;
            _Y;
            pa_atomic_inc(&l->read_idx);
            _Y;
            p = cells[idx].data;
            _Y;
            pa_atomic_store(&cells[idx].state, STATE_UNUSED);
            _Y;

            pa_atomic_dec(&l->length);
            return p;
        }

        _Y;
        idx = reduce(l, idx+1);
    }

#ifdef PROFILE
    if (len > N_EXTRA_SCAN)
        pa_log_warn("Didn't find used cell after %u iterations.", len);
#endif

    return NULL;
}
