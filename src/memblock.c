/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
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

static unsigned memblock_count = 0, memblock_total = 0;

struct pa_memblock *pa_memblock_new(size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock)+length);
    b->type = PA_MEMBLOCK_APPENDED;
    b->ref = 1;
    b->length = length;
    b->data = b+1;
    memblock_count++;
    memblock_total += length;
    return b;
}

struct pa_memblock *pa_memblock_new_fixed(void *d, size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_FIXED;
    b->ref = 1;
    b->length = length;
    b->data = d;
    memblock_count++;
    memblock_total += length;
    return b;
}

struct pa_memblock *pa_memblock_new_dynamic(void *d, size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_DYNAMIC;
    b->ref = 1;
    b->length = length;
    b->data = d;
    memblock_count++;
    memblock_total += length;
    return b;
}

struct pa_memblock* pa_memblock_ref(struct pa_memblock*b) {
    assert(b && b->ref >= 1);
    b->ref++;
    return b;
}

void pa_memblock_unref(struct pa_memblock*b) {
    assert(b && b->ref >= 1);
    b->ref--;

    if (b->ref == 0) {
        if (b->type == PA_MEMBLOCK_DYNAMIC)
            free(b->data);

        memblock_count--;
        memblock_total -= b->length;

        free(b);
    }
}

void pa_memblock_unref_fixed(struct pa_memblock *b) {
    void *d;
    
    assert(b && b->ref >= 1);

    if (b->ref == 1) {
        pa_memblock_unref(b);
        return;
    } else {
        d = malloc(b->length);
        assert(d);
        memcpy(d, b->data, b->length);
        b->data = d;
        b->type = PA_MEMBLOCK_DYNAMIC;
        b->ref--;
    }
}

unsigned pa_memblock_get_count(void) {
    return memblock_count;
}

unsigned pa_memblock_get_total(void) {
    return memblock_total;
}
