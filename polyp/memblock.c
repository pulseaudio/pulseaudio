/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "memblock.h"
#include "xmalloc.h"

static void stat_add(struct pa_memblock*m, struct pa_memblock_stat *s) {
    assert(m);

    m->stat = pa_memblock_stat_ref(s);
    s->total++;
    s->allocated++;
    s->total_size += m->length;
    s->allocated_size += m->length;
}

static void stat_remove(struct pa_memblock *m) {
    assert(m);

    if (!m->stat)
        return;

    m->stat->total--;
    m->stat->total_size -= m->length;
    
    pa_memblock_stat_unref(m->stat);
    m->stat = NULL;
}

struct pa_memblock *pa_memblock_new(size_t length, struct pa_memblock_stat*s) {
    struct pa_memblock *b = pa_xmalloc(sizeof(struct pa_memblock)+length);
    b->type = PA_MEMBLOCK_APPENDED;
    b->ref = 1;
    b->length = length;
    b->data = b+1;
    b->free_cb = NULL;
    stat_add(b, s);
    return b;
}

struct pa_memblock *pa_memblock_new_fixed(void *d, size_t length, struct pa_memblock_stat*s) {
    struct pa_memblock *b = pa_xmalloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_FIXED;
    b->ref = 1;
    b->length = length;
    b->data = d;
    b->free_cb = NULL;
    stat_add(b, s);
    return b;
}

struct pa_memblock *pa_memblock_new_dynamic(void *d, size_t length, struct pa_memblock_stat*s) {
    struct pa_memblock *b = pa_xmalloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_DYNAMIC;
    b->ref = 1;
    b->length = length;
    b->data = d;
    b->free_cb = NULL;
    stat_add(b, s);
    return b;
}

struct pa_memblock *pa_memblock_new_user(void *d, size_t length, void (*free_cb)(void *p), struct pa_memblock_stat*s) {
    struct pa_memblock *b;
    assert(d && length && free_cb);
    b = pa_xmalloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_USER;
    b->ref = 1;
    b->length = length;
    b->data = d;
    b->free_cb = free_cb;
    stat_add(b, s);
    return b;
}

struct pa_memblock* pa_memblock_ref(struct pa_memblock*b) {
    assert(b && b->ref >= 1);
    b->ref++;
    return b;
}

void pa_memblock_unref(struct pa_memblock*b) {
    assert(b && b->ref >= 1);

    if ((--(b->ref)) == 0) {
        stat_remove(b);

        if (b->type == PA_MEMBLOCK_USER) {
            assert(b->free_cb);
            b->free_cb(b->data);
        } else if (b->type == PA_MEMBLOCK_DYNAMIC)
            pa_xfree(b->data);

        pa_xfree(b);
    }
}

void pa_memblock_unref_fixed(struct pa_memblock *b) {
    assert(b && b->ref >= 1 && b->type == PA_MEMBLOCK_FIXED);

    if (b->ref == 1)
        pa_memblock_unref(b);
    else {
        b->data = pa_xmemdup(b->data, b->length);
        b->type = PA_MEMBLOCK_DYNAMIC;
        b->ref--;
    }
}

struct pa_memblock_stat* pa_memblock_stat_new(void) {
    struct pa_memblock_stat *s;

    s = pa_xmalloc(sizeof(struct pa_memblock_stat));
    s->ref = 1;
    s->total = s->total_size = s->allocated = s->allocated_size = 0;

    return s;
}

void pa_memblock_stat_unref(struct pa_memblock_stat *s) {
    assert(s && s->ref >= 1);

    if (!(--(s->ref))) {
        assert(!s->total);
        pa_xfree(s);
    }
}

struct pa_memblock_stat * pa_memblock_stat_ref(struct pa_memblock_stat *s) {
    assert(s);
    s->ref++;
    return s;
}
