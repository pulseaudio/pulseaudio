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

#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "memblockq.h"
#include "xmalloc.h"
#include "log.h"

struct memblock_list {
    struct memblock_list *next, *prev;
    struct pa_memchunk chunk;
};

struct pa_memblockq {
    struct memblock_list *blocks, *blocks_tail;
    unsigned n_blocks;
    size_t current_length, maxlength, tlength, base, prebuf, orig_prebuf, minreq;
    struct pa_mcalign *mcalign;
    struct pa_memblock_stat *memblock_stat;
};

struct pa_memblockq* pa_memblockq_new(size_t maxlength, size_t tlength, size_t base, size_t prebuf, size_t minreq, struct pa_memblock_stat *s) {
    struct pa_memblockq* bq;
    assert(maxlength && base && maxlength);
    
    bq = pa_xmalloc(sizeof(struct pa_memblockq));
    bq->blocks = bq->blocks_tail = 0;
    bq->n_blocks = 0;

    bq->current_length = 0;

    /*pa_log(__FILE__": memblockq requested: maxlength=%u, tlength=%u, base=%u, prebuf=%u, minreq=%u\n", maxlength, tlength, base, prebuf, minreq);*/
    
    bq->base = base;

    bq->maxlength = ((maxlength+base-1)/base)*base;
    assert(bq->maxlength >= base);

    bq->tlength = ((tlength+base-1)/base)*base;
    if (!bq->tlength || bq->tlength >= bq->maxlength)
        bq->tlength = bq->maxlength;
    
    bq->prebuf = (prebuf == (size_t) -1) ? bq->maxlength/2 : prebuf;
    bq->prebuf = (bq->prebuf/base)*base;
    if (bq->prebuf > bq->maxlength)
        bq->prebuf = bq->maxlength;

    bq->orig_prebuf = bq->prebuf;
    
    bq->minreq = (minreq/base)*base;
    if (bq->minreq == 0)
        bq->minreq = 1;

    pa_log(__FILE__": memblockq sanitized: maxlength=%u, tlength=%u, base=%u, prebuf=%u, minreq=%u\n", bq->maxlength, bq->tlength, bq->base, bq->prebuf, bq->minreq);
    
    bq->mcalign = NULL;

    bq->memblock_stat = s;

    return bq;
}

void pa_memblockq_free(struct pa_memblockq* bq) {
    assert(bq);

    pa_memblockq_flush(bq);
    
    if (bq->mcalign)
        pa_mcalign_free(bq->mcalign);

    pa_xfree(bq);
}

void pa_memblockq_push(struct pa_memblockq* bq, const struct pa_memchunk *chunk, size_t delta) {
    struct memblock_list *q;
    assert(bq && chunk && chunk->memblock && chunk->length && (chunk->length % bq->base) == 0);

    pa_memblockq_seek(bq, delta);
    
    if (bq->blocks_tail && bq->blocks_tail->chunk.memblock == chunk->memblock) {
        /* Try to merge memory chunks */

        if (bq->blocks_tail->chunk.index+bq->blocks_tail->chunk.length == chunk->index) {
            bq->blocks_tail->chunk.length += chunk->length;
            bq->current_length += chunk->length;
            return;
        }
    }
    
    q = pa_xmalloc(sizeof(struct memblock_list));

    q->chunk = *chunk;
    pa_memblock_ref(q->chunk.memblock);
    assert(q->chunk.index+q->chunk.length <= q->chunk.memblock->length);
    q->next = NULL;
    if ((q->prev = bq->blocks_tail))
        bq->blocks_tail->next = q;
    else
        bq->blocks = q;
    
    bq->blocks_tail = q;

    bq->n_blocks++;
    bq->current_length += chunk->length;

    pa_memblockq_shorten(bq, bq->maxlength);
}

int pa_memblockq_peek(struct pa_memblockq* bq, struct pa_memchunk *chunk) {
    assert(bq && chunk);

    if (!bq->blocks || bq->current_length < bq->prebuf)
        return -1;

    bq->prebuf = 0;

    *chunk = bq->blocks->chunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

void pa_memblockq_drop(struct pa_memblockq *bq, const struct pa_memchunk *chunk, size_t length) {
    assert(bq && chunk && length);

    if (!bq->blocks || memcmp(&bq->blocks->chunk, chunk, sizeof(struct pa_memchunk))) 
        return;
    
    assert(length <= bq->blocks->chunk.length);
    pa_memblockq_skip(bq, length);
}

static void remove_block(struct pa_memblockq *bq, struct memblock_list *q) {
    assert(bq && q);

    if (q->prev)
        q->prev->next = q->next;
    else {
        assert(bq->blocks == q);
        bq->blocks = q->next;
    }
    
    if (q->next)
        q->next->prev = q->prev;
    else {
        assert(bq->blocks_tail == q);
        bq->blocks_tail = q->prev;
    }
    
    pa_memblock_unref(q->chunk.memblock);
    pa_xfree(q);
    
    bq->n_blocks--;
}

void pa_memblockq_skip(struct pa_memblockq *bq, size_t length) {
    assert(bq && length && (length % bq->base) == 0);

    while (length > 0) {
        size_t l = length;
        assert(bq->blocks && bq->current_length >= length);
        
        if (l > bq->blocks->chunk.length)
            l = bq->blocks->chunk.length;

        bq->blocks->chunk.index += l;
        bq->blocks->chunk.length -= l;
        bq->current_length -= l;
        
        if (!bq->blocks->chunk.length)
            remove_block(bq, bq->blocks);

        length -= l;
    }
}

void pa_memblockq_shorten(struct pa_memblockq *bq, size_t length) {
    size_t l;
    assert(bq);

    if (bq->current_length <= length)
        return;

    /*pa_log(__FILE__": Warning! pa_memblockq_shorten()\n");*/
    
    l = bq->current_length - length;
    l /= bq->base;
    l *= bq->base;

    pa_memblockq_skip(bq, l);
}


void pa_memblockq_empty(struct pa_memblockq *bq) {
    assert(bq);
    pa_memblockq_shorten(bq, 0);
}

int pa_memblockq_is_readable(struct pa_memblockq *bq) {
    assert(bq);

    return bq->current_length && (bq->current_length >= bq->prebuf);
}

int pa_memblockq_is_writable(struct pa_memblockq *bq, size_t length) {
    assert(bq);

    return bq->current_length + length <= bq->tlength;
}

uint32_t pa_memblockq_get_length(struct pa_memblockq *bq) {
    assert(bq);
    return bq->current_length;
}

uint32_t pa_memblockq_missing(struct pa_memblockq *bq) {
    size_t l;
    assert(bq);

    if (bq->current_length >= bq->tlength)
        return 0;

    l = bq->tlength - bq->current_length;
    assert(l);

    return (l >= bq->minreq) ? l : 0;
}

void pa_memblockq_push_align(struct pa_memblockq* bq, const struct pa_memchunk *chunk, size_t delta) {
    struct pa_memchunk rchunk;
    assert(bq && chunk && bq->base);

    if (bq->base == 1) {
        pa_memblockq_push(bq, chunk, delta);
        return;
    }

    if (!bq->mcalign) {
        bq->mcalign = pa_mcalign_new(bq->base, bq->memblock_stat);
        assert(bq->mcalign);
    }
    
    pa_mcalign_push(bq->mcalign, chunk);

    while (pa_mcalign_pop(bq->mcalign, &rchunk) >= 0) {
        pa_memblockq_push(bq, &rchunk, delta);
        pa_memblock_unref(rchunk.memblock);
        delta = 0;
    }
}

uint32_t pa_memblockq_get_minreq(struct pa_memblockq *bq) {
    assert(bq);
    return bq->minreq;
}

void pa_memblockq_prebuf_disable(struct pa_memblockq *bq) {
    assert(bq);
    bq->prebuf = 0;
}

void pa_memblockq_prebuf_reenable(struct pa_memblockq *bq) {
    assert(bq);
    bq->prebuf = bq->orig_prebuf;
}

void pa_memblockq_seek(struct pa_memblockq *bq, size_t length) {
    assert(bq);

    if (!length)
        return;

    while (length >= bq->base) {
        size_t l = length;
        if (!bq->current_length)
            return;

        assert(bq->blocks_tail);
        
        if (l > bq->blocks_tail->chunk.length)
            l = bq->blocks_tail->chunk.length;

        bq->blocks_tail->chunk.length -= l;
        bq->current_length -= l;
        
        if (bq->blocks_tail->chunk.length == 0)
            remove_block(bq, bq->blocks);

        length -= l;
    }
}

void pa_memblockq_flush(struct pa_memblockq *bq) {
    struct memblock_list *l;
    assert(bq);
    
    while ((l = bq->blocks)) {
        bq->blocks = l->next;
        pa_memblock_unref(l->chunk.memblock);
        pa_xfree(l);
    }

    bq->blocks_tail = NULL;
    bq->n_blocks = 0;
    bq->current_length = 0;
}

uint32_t pa_memblockq_get_tlength(struct pa_memblockq *bq) {
    assert(bq);
    return bq->tlength;
}
