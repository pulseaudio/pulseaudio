/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

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

#include <string.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>

#include "dynarray.h"

/* If the array becomes to small, increase its size by 25 entries */
#define INCREASE_BY 25

struct pa_dynarray {
    void **data;
    unsigned n_allocated, n_entries;
};

pa_dynarray* pa_dynarray_new(void) {
    pa_dynarray *a;

    a = pa_xnew(pa_dynarray, 1);
    a->data = NULL;
    a->n_entries = 0;
    a->n_allocated = 0;

    return a;
}

void pa_dynarray_free(pa_dynarray *a, pa_free_cb_t free_func) {
    unsigned i;
    pa_assert(a);

    if (free_func)
        for (i = 0; i < a->n_entries; i++)
            if (a->data[i])
                free_func(a->data[i]);

    pa_xfree(a->data);
    pa_xfree(a);
}

void pa_dynarray_put(pa_dynarray*a, unsigned i, void *p) {
    pa_assert(a);

    if (i >= a->n_allocated) {
        unsigned n;

        if (!p)
            return;

        n = i+INCREASE_BY;
        a->data = pa_xrealloc(a->data, sizeof(void*)*n);
        memset(a->data+a->n_allocated, 0, sizeof(void*)*(n-a->n_allocated));
        a->n_allocated = n;
    }

    a->data[i] = p;

    if (i >= a->n_entries)
        a->n_entries = i+1;
}

unsigned pa_dynarray_append(pa_dynarray*a, void *p) {
    unsigned i;

    pa_assert(a);

    i = a->n_entries;
    pa_dynarray_put(a, i, p);

    return i;
}

void *pa_dynarray_get(pa_dynarray*a, unsigned i) {
    pa_assert(a);

    if (i >= a->n_entries)
        return NULL;

    pa_assert(a->data);
    return a->data[i];
}

unsigned pa_dynarray_size(pa_dynarray*a) {
    pa_assert(a);

    return a->n_entries;
}
