/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering
  Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).

  Contact: Jyri Sarha <Jyri.Sarha@nokia.com>

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
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "flist.h"

#define FLIST_SIZE 128

/* Lock free single linked list element. */
struct pa_flist_elem {
    pa_atomic_ptr_t next;
    pa_atomic_ptr_t ptr;
};

typedef struct pa_flist_elem pa_flist_elem;

struct pa_flist {
    const char *name;
    unsigned size;
    /* Stack that contains pointers stored into free list */
    pa_atomic_ptr_t stored;
    /* Stack that contains empty list elements */
    pa_atomic_ptr_t empty;
    pa_flist_elem table[0];
};

/* Lock free pop from linked list stack */
static pa_flist_elem *stack_pop(pa_atomic_ptr_t *list) {
    pa_flist_elem *poped;
    pa_assert(list);

    do {
        poped = (pa_flist_elem *) pa_atomic_ptr_load(list);
    } while (poped != NULL && !pa_atomic_ptr_cmpxchg(list, poped, pa_atomic_ptr_load(&poped->next)));

    return poped;
}

/* Lock free push to linked list stack */
static void stack_push(pa_atomic_ptr_t *list, pa_flist_elem *new_elem) {
    pa_flist_elem *next;
    pa_assert(list);

    do {
        next = pa_atomic_ptr_load(list);
        pa_atomic_ptr_store(&new_elem->next, next);
    } while (!pa_atomic_ptr_cmpxchg(list, next, new_elem));
}

pa_flist *pa_flist_new_with_name(unsigned size, const char *name) {
    pa_flist *l;
    unsigned i;
    pa_assert(name);

    if (!size)
        size = FLIST_SIZE;

    l = pa_xmalloc0(sizeof(pa_flist) + sizeof(pa_flist_elem) * size);

    l->name = pa_xstrdup(name);
    l->size = size;
    pa_atomic_ptr_store(&l->stored, NULL);
    pa_atomic_ptr_store(&l->empty, NULL);
    for (i=0; i < size; i++) {
        stack_push(&l->empty, &l->table[i]);
    }
    return l;
}

pa_flist *pa_flist_new(unsigned size) {
    return pa_flist_new_with_name(size, "unknown");
}

void pa_flist_free(pa_flist *l, pa_free_cb_t free_cb) {
    pa_assert(l);
    pa_assert(l->name);

    if (free_cb) {
        pa_flist_elem *elem;
        while((elem = stack_pop(&l->stored)))
            free_cb(pa_atomic_ptr_load(&elem->ptr));
    }

    pa_xfree(l);
}

int pa_flist_push(pa_flist *l, void *p) {
    pa_flist_elem *elem;
    pa_assert(l);
    pa_assert(p);

    elem = stack_pop(&l->empty);
    if (elem == NULL) {
        if (pa_log_ratelimit(PA_LOG_DEBUG))
            pa_log_debug("%s flist is full (don't worry)", l->name);
        return -1;
    }
    pa_atomic_ptr_store(&elem->ptr, p);
    stack_push(&l->stored, elem);

    return 0;
}

void* pa_flist_pop(pa_flist *l) {
    pa_flist_elem *elem;
    void *ptr;
    pa_assert(l);

    elem = stack_pop(&l->stored);
    if (elem == NULL)
        return NULL;

    ptr = pa_atomic_ptr_load(&elem->ptr);

    stack_push(&l->empty, elem);

    return ptr;
}
