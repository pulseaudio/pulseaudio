/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

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

#include <pulse/xmalloc.h>

#include <pulsecore/flist.h>

#include "prioq.h"

struct pa_prioq_item {
    void *value;
    unsigned idx;
};

struct pa_prioq {
    pa_prioq_item **items;
    unsigned n_items;
    unsigned n_allocated;
    pa_compare_func_t compare_func;
};

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

pa_prioq *pa_prioq_new(pa_compare_func_t compare_func) {

    pa_prioq *q;

    q = pa_xnew(pa_prioq, 1);
    q->compare_func = compare_func;
    q->n_items = 0;
    q->n_allocated = 64;
    q->items = pa_xnew(pa_prioq_item*, q->n_allocated);

    return q;
}

void pa_prioq_free(pa_prioq *q, pa_free2_cb_t free_cb, void *userdata) {
    pa_prioq_item **i, **e;

    pa_assert(q);

    for (i = q->items, e = q->items + q->n_items; i < e; i++) {

        if (!*i)
            continue;

        if (free_cb)
            free_cb((*i)->value, userdata);

        pa_xfree(*i);
    }

    pa_xfree(q->items);
    pa_xfree(q);
}

static void shuffle_up(pa_prioq *q, pa_prioq_item *i) {
    unsigned j;

    pa_assert(q);
    pa_assert(i);

    j = i->idx;

    while (j > 0) {
        unsigned k;

        k = (j-1)/2;

        if (q->compare_func(q->items[k]->value, i->value) < 0)
            break;

        q->items[k]->idx = j;
        q->items[j] = q->items[k];

        j = k;
    }

    i->idx = j;
    q->items[j] = i;

}

pa_prioq_item* pa_prioq_put(pa_prioq *q, void *p) {
    pa_prioq_item *i;

    pa_assert(q);

    if (q->n_items >= q->n_allocated) {
        q->n_allocated = PA_MAX(q->n_items+1, q->n_allocated)*2;
        q->items = pa_xrealloc(q->items, sizeof(pa_prioq_item*) * q->n_allocated);
    }

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(pa_prioq_item, 1);

    i->value = p;
    i->idx = q->n_items++;

    shuffle_up(q, i);

    return i;
}

void* pa_prioq_peek(pa_prioq *q) {
    pa_assert(q);

    if (q->n_items <= 0)
        return NULL;

    return q->items[0]->value;
}

void* pa_prioq_pop(pa_prioq *q){
    pa_assert(q);

    if (q->n_items <= 0)
        return NULL;

    return pa_prioq_remove(q, q->items[0]);
}

static void swap(pa_prioq *q, unsigned j, unsigned k) {
    pa_prioq_item *t;

    pa_assert(q);
    pa_assert(j < q->n_items);
    pa_assert(k < q->n_items);

    pa_assert(q->items[j]->idx == j);
    pa_assert(q->items[k]->idx == k);

    t = q->items[j];

    q->items[j]->idx = k;
    q->items[j] = q->items[k];

    q->items[k]->idx = j;
    q->items[k] = t;
}

static void shuffle_down(pa_prioq *q, unsigned idx) {

    pa_assert(q);
    pa_assert(idx < q->n_items);

    for (;;) {
        unsigned j, k, s;

        k = (idx+1)*2; /* right child */
        j = k-1;       /* left child */

        if (j >= q->n_items)
            break;

        if (q->compare_func(q->items[j]->value, q->items[idx]->value) < 0)

            /* So our left child is smaller than we are, let's
             * remember this fact */
            s = j;
        else
            s = idx;

        if (k < q->n_items &&
            q->compare_func(q->items[k]->value, q->items[s]->value) < 0)

            /* So our right child is smaller than we are, let's
             * remember this fact */
            s = k;

        /* s now points to the smallest of the three items */

        if (s == idx)
            /* No swap necessary, we're done */
            break;

        swap(q, idx, s);
        idx = s;
    }
}

void* pa_prioq_remove(pa_prioq *q, pa_prioq_item *i) {
    void *p;

    pa_assert(q);
    pa_assert(i);
    pa_assert(q->n_items >= 1);

    p = i->value;

    if (q->n_items-1 == i->idx) {
        /* We are the last entry, so let's just remove us and good */
        q->n_items--;

    } else {

        /* We are not the last entry, we need to replace ourselves
         * with the last node and reshuffle */

        q->items[i->idx] = q->items[q->n_items-1];
        q->items[i->idx]->idx = i->idx;
        q->n_items--;

        shuffle_down(q, i->idx);
    }

    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    return p;
}

unsigned pa_prioq_size(pa_prioq *q) {
    pa_assert(q);

    return q->n_items;
}

pa_bool_t pa_prioq_isempty(pa_prioq *q) {
    pa_assert(q);

    return q->n_items == 0;
}

void pa_prioq_reshuffle(pa_prioq *q, pa_prioq_item *i) {
    pa_assert(q);
    pa_assert(i);

    /* This will move the entry down as far as necessary */
    shuffle_down(q, i->idx);

    /* And this will move the entry up as far as necessary */
    shuffle_up(q, i);
}
