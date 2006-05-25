/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
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

#include <polyp/xmalloc.h>

#include <polypcore/log.h>
#include <polypcore/mcalign.h>

#include "memblockq.h"

struct memblock_list {
    struct memblock_list *next, *prev;
    int64_t index;
    pa_memchunk chunk;
};

struct pa_memblockq {
    struct memblock_list *blocks, *blocks_tail;
    unsigned n_blocks;
    size_t maxlength, tlength, base, prebuf, minreq;
    int64_t read_index, write_index;
    enum { PREBUF, RUNNING } state;
    pa_memblock_stat *memblock_stat;
    pa_memblock *silence;
    pa_mcalign *mcalign;
};

pa_memblockq* pa_memblockq_new(
        int64_t idx,
        size_t maxlength,
        size_t tlength,
        size_t base,
        size_t prebuf,
        size_t minreq,
        pa_memblock *silence,
        pa_memblock_stat *s) {
    
    pa_memblockq* bq;
    
    assert(base > 0);
    assert(maxlength >= base);
    
    bq = pa_xnew(pa_memblockq, 1);
    bq->blocks = bq->blocks_tail = NULL;
    bq->n_blocks = 0;

    bq->base = base;
    bq->read_index = bq->write_index = idx;
    bq->memblock_stat = s;

    pa_log_debug(__FILE__": memblockq requested: maxlength=%lu, tlength=%lu, base=%lu, prebuf=%lu, minreq=%lu",
        (unsigned long)maxlength, (unsigned long)tlength, (unsigned long)base, (unsigned long)prebuf, (unsigned long)minreq);

    bq->maxlength = ((maxlength+base-1)/base)*base;
    assert(bq->maxlength >= base);

    bq->tlength = ((tlength+base-1)/base)*base;
    if (!bq->tlength || bq->tlength >= bq->maxlength)
        bq->tlength = bq->maxlength;

    bq->prebuf = (prebuf == (size_t) -1) ? bq->tlength/2 : prebuf;
    bq->prebuf = ((bq->prebuf+base-1)/base)*base;
    if (bq->prebuf > bq->maxlength)
        bq->prebuf = bq->maxlength;

    bq->minreq = (minreq/base)*base;
    
    if (bq->minreq > bq->tlength - bq->prebuf)
        bq->minreq = bq->tlength - bq->prebuf;

    if (!bq->minreq)
        bq->minreq = 1;
    
    pa_log_debug(__FILE__": memblockq sanitized: maxlength=%lu, tlength=%lu, base=%lu, prebuf=%lu, minreq=%lu",
        (unsigned long)bq->maxlength, (unsigned long)bq->tlength, (unsigned long)bq->base, (unsigned long)bq->prebuf, (unsigned long)bq->minreq);

    bq->state = bq->prebuf ? PREBUF : RUNNING;
    bq->silence = silence ? pa_memblock_ref(silence) : NULL;
    bq->mcalign = NULL;
    
    return bq;
}

void pa_memblockq_free(pa_memblockq* bq) {
    assert(bq);

    pa_memblockq_flush(bq);

    if (bq->silence)
        pa_memblock_unref(bq->silence);

    if (bq->mcalign)
        pa_mcalign_free(bq->mcalign);
    
    pa_xfree(bq);
}

static void drop_block(pa_memblockq *bq, struct memblock_list *q) {
    assert(bq);
    assert(q);

    assert(bq->n_blocks >= 1);
    
    if (q->prev)
        q->prev->next = q->next;
    else
        bq->blocks = q->next;
    
    if (q->next)
        q->next->prev = q->prev;
    else
        bq->blocks_tail = q->prev;

    pa_memblock_unref(q->chunk.memblock);
    pa_xfree(q);

    bq->n_blocks--;
}

static int can_push(pa_memblockq *bq, size_t l) {
    int64_t end;

    assert(bq);

    if (bq->read_index > bq->write_index) {
        size_t d =  bq->read_index - bq->write_index;

        if (l > d)
            l -= d;
        else
            return 1;
    }

    end = bq->blocks_tail ? bq->blocks_tail->index + bq->blocks_tail->chunk.length : 0;

    /* Make sure that the list doesn't get too long */
    if (bq->write_index + (int64_t)l > end)
        if (bq->write_index + l - bq->read_index > bq->maxlength)
            return 0;

    return 1;
}

int pa_memblockq_push(pa_memblockq* bq, const pa_memchunk *uchunk) {
    
    struct memblock_list *q, *n;
    pa_memchunk chunk;
    
    assert(bq);
    assert(uchunk);
    assert(uchunk->memblock);
    assert(uchunk->length > 0);
    assert(uchunk->index + uchunk->length <= uchunk->memblock->length);

    if (uchunk->length % bq->base)
        return -1;

    if (!can_push(bq, uchunk->length))
        return -1;

    chunk = *uchunk;
    
    if (bq->read_index > bq->write_index) {

        /* We currently have a buffer underflow, we need to drop some
         * incoming data */

        size_t d = bq->read_index - bq->write_index;

        if (chunk.length > d) {
            chunk.index += d;
            chunk.length -= d;
            bq->write_index = bq->read_index;
        } else {
            /* We drop the incoming data completely */
            bq->write_index += chunk.length;
            return 0;
        }
    }
    
    /* We go from back to front to look for the right place to add
     * this new entry. Drop data we will overwrite on the way */

    q = bq->blocks_tail;
    while (q) {

        if (bq->write_index >= q->index + (int64_t)q->chunk.length)
            /* We found the entry where we need to place the new entry immediately after */
            break;
        else if (bq->write_index + (int64_t)chunk.length <= q->index) {
            /* This entry isn't touched at all, let's skip it */
            q = q->prev;
        } else if (bq->write_index <= q->index &&
            bq->write_index + chunk.length >= q->index + q->chunk.length) {

            /* This entry is fully replaced by the new entry, so let's drop it */

            struct memblock_list *p;
            p = q;
            q = q->prev;
            drop_block(bq, p);
        } else if (bq->write_index >= q->index) {
            /* The write index points into this memblock, so let's
             * truncate or split it */

            if (bq->write_index + chunk.length < q->index + q->chunk.length) {

                /* We need to save the end of this memchunk */
                struct memblock_list *p;
                size_t d;

                /* Create a new list entry for the end of thie memchunk */
                p = pa_xnew(struct memblock_list, 1);
                p->chunk = q->chunk;
                pa_memblock_ref(p->chunk.memblock);

                /* Calculate offset */
                d = bq->write_index + chunk.length - q->index;
                assert(d > 0);

                /* Drop it from the new entry */
                p->index = q->index + d;
                p->chunk.length -= d;

                /* Add it to the list */
                p->prev = q;
                if ((p->next = q->next))
                    q->next->prev = p;
                else
                    bq->blocks_tail = p;
                q->next = p;

                bq->n_blocks++;
            }

            /* Truncate the chunk */
            if (!(q->chunk.length = bq->write_index - q->index)) {
                struct memblock_list *p;
                p = q;
                q = q->prev;
                drop_block(bq, p);
            }

            /* We had to truncate this block, hence we're now at the right position */
            break;
        } else {
            size_t d;

            assert(bq->write_index + (int64_t)chunk.length > q->index &&
                   bq->write_index + (int64_t)chunk.length < q->index + (int64_t)q->chunk.length &&
                   bq->write_index < q->index);
            
            /* The job overwrites the current entry at the end, so let's drop the beginning of this entry */

            d = bq->write_index + chunk.length - q->index;
            q->index += d;
            q->chunk.index += d;
            q->chunk.length -= d;
            
            q = q->prev;
        }
        
    }

    if (q) {
        assert(bq->write_index >=  q->index + (int64_t)q->chunk.length);
        assert(!q->next || (bq->write_index + (int64_t)chunk.length <= q->next->index));
               
        /* Try to merge memory blocks */
        
        if (q->chunk.memblock == chunk.memblock &&
            q->chunk.index + (int64_t)q->chunk.length == chunk.index &&
            bq->write_index == q->index + (int64_t)q->chunk.length) {
            
            q->chunk.length += chunk.length;
            bq->write_index += chunk.length;
            return 0;
        }
    } else
        assert(!bq->blocks || (bq->write_index + (int64_t)chunk.length <= bq->blocks->index));


    n = pa_xnew(struct memblock_list, 1);
    n->chunk = chunk;
    pa_memblock_ref(n->chunk.memblock);
    n->index = bq->write_index;
    bq->write_index += n->chunk.length;

    n->next = q ? q->next : bq->blocks;
    n->prev = q;

    if (n->next)
        n->next->prev = n;
    else
        bq->blocks_tail = n;

    if (n->prev)
        n->prev->next = n;
    else
        bq->blocks = n;
    
    bq->n_blocks++;
    return 0;
}

int pa_memblockq_peek(pa_memblockq* bq, pa_memchunk *chunk) {
    assert(bq);
    assert(chunk);

    if (bq->state == PREBUF) {

        /* We need to pre-buffer */
        if (pa_memblockq_get_length(bq) < bq->prebuf)
            return -1;

        bq->state = RUNNING;

    } else if (bq->prebuf > 0 && bq->read_index >= bq->write_index) {

        /* Buffer underflow protection */
        bq->state = PREBUF;
        return -1;
    }
    
    /* Do we need to spit out silence? */
    if (!bq->blocks || bq->blocks->index > bq->read_index) {

        size_t length;

        /* How much silence shall we return? */
        length = bq->blocks ? bq->blocks->index - bq->read_index : 0;

        /* We need to return silence, since no data is yet available */
        if (bq->silence) {
            chunk->memblock = pa_memblock_ref(bq->silence);

            if (!length || length > chunk->memblock->length)
                length = chunk->memblock->length;
                
            chunk->length = length;
        } else {
            chunk->memblock = NULL;
            chunk->length = length;
        }

        chunk->index = 0;
        return 0;
    }

    /* Ok, let's pass real data to the caller */
    assert(bq->blocks->index == bq->read_index);
    
    *chunk = bq->blocks->chunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

void pa_memblockq_drop(pa_memblockq *bq, const pa_memchunk *chunk, size_t length) {
    assert(bq);
    assert(length % bq->base == 0);

    assert(!chunk || length <= chunk->length);

    if (chunk) {

        if (bq->blocks && bq->blocks->index == bq->read_index) {
            /* The first item in queue is valid */

            /* Does the chunk match with what the user supplied us? */
            if (memcmp(chunk, &bq->blocks->chunk, sizeof(pa_memchunk)) != 0)
                return;

        } else {
            size_t l;

            /* The first item in the queue is not yet relevant */

            assert(!bq->blocks || bq->blocks->index > bq->read_index);
            l = bq->blocks ? bq->blocks->index - bq->read_index : 0;

            if (bq->silence) {

                if (!l || l > bq->silence->length)
                    l = bq->silence->length;

            }

            /* Do the entries still match? */
            if (chunk->index != 0 || chunk->length != l || chunk->memblock != bq->silence)
                return;
        }
    }

    while (length > 0) {

        if (bq->blocks) {
            size_t d;

            assert(bq->blocks->index >= bq->read_index);

            d = (size_t) (bq->blocks->index - bq->read_index);
            
            if (d >= length) {
                /* The first block is too far in the future */
                
                bq->read_index += length;
                break;
            } else {
                
                length -= d;
                bq->read_index += d;
            }

            assert(bq->blocks->index == bq->read_index);

            if (bq->blocks->chunk.length <= length) {
                /* We need to drop the full block */

                length -= bq->blocks->chunk.length;
                bq->read_index += bq->blocks->chunk.length;

                drop_block(bq, bq->blocks);
            } else {
                /* Only the start of this block needs to be dropped */

                bq->blocks->chunk.index += length;
                bq->blocks->chunk.length -= length;
                bq->blocks->index += length;
                bq->read_index += length;
                break;
            }
            
        } else {

            /* The list is empty, there's nothing we could drop */
            bq->read_index += length;
            break;
        }
    }
}

int pa_memblockq_is_readable(pa_memblockq *bq) {
    assert(bq);

    if (bq->prebuf > 0) {
        size_t l = pa_memblockq_get_length(bq);
        
        if (bq->state == PREBUF && l < bq->prebuf)
            return 0;

        if (l <= 0)
            return 0;
    }

    return 1;
}

int pa_memblockq_is_writable(pa_memblockq *bq, size_t length) {
    assert(bq);

    if (length % bq->base)
        return 0;
    
    return pa_memblockq_get_length(bq) + length <= bq->tlength;
}

size_t pa_memblockq_get_length(pa_memblockq *bq) {
    assert(bq);

    if (bq->write_index <= bq->read_index)
        return 0;
    
    return (size_t) (bq->write_index - bq->read_index);
}

size_t pa_memblockq_missing(pa_memblockq *bq) {
    size_t l;
    assert(bq);

    if ((l = pa_memblockq_get_length(bq)) >= bq->tlength)
        return 0;

    l = bq->tlength - l;
    return (l >= bq->minreq) ? l : 0;
}

size_t pa_memblockq_get_minreq(pa_memblockq *bq) {
    assert(bq);

    return bq->minreq;
}

void pa_memblockq_seek(pa_memblockq *bq, int64_t offset, pa_seek_mode_t seek) {
    assert(bq);

    switch (seek) {
        case PA_SEEK_RELATIVE:
            bq->write_index += offset;
            return;
        case PA_SEEK_ABSOLUTE:
            bq->write_index = offset;
            return;
        case PA_SEEK_RELATIVE_ON_READ:
            bq->write_index = bq->read_index + offset;
            return;
        case PA_SEEK_RELATIVE_END:
            bq->write_index = (bq->blocks_tail ? bq->blocks_tail->index + (int64_t)bq->blocks_tail->chunk.length : bq->read_index) + offset;
            return;
    }

    assert(0);
}

void pa_memblockq_flush(pa_memblockq *bq) {
    assert(bq);
    
    while (bq->blocks)
        drop_block(bq, bq->blocks);

    assert(bq->n_blocks == 0);

    bq->write_index = bq->read_index;

    pa_memblockq_prebuf_force(bq);
}

size_t pa_memblockq_get_tlength(pa_memblockq *bq) {
    assert(bq);
    
    return bq->tlength;
}

int64_t pa_memblockq_get_read_index(pa_memblockq *bq) {
    assert(bq);
    return bq->read_index;
}

int64_t pa_memblockq_get_write_index(pa_memblockq *bq) {
    assert(bq);
    return bq->write_index;
}

int pa_memblockq_push_align(pa_memblockq* bq, const pa_memchunk *chunk) {
    pa_memchunk rchunk;

    assert(bq);
    assert(chunk && bq->base);
 	
    if (bq->base == 1)
        return pa_memblockq_push(bq, chunk);
 	
    if (!bq->mcalign)
        bq->mcalign = pa_mcalign_new(bq->base, bq->memblock_stat);

    if (!can_push(bq, pa_mcalign_csize(bq->mcalign, chunk->length)))
        return -1;
    
    pa_mcalign_push(bq->mcalign, chunk);
 	
    while (pa_mcalign_pop(bq->mcalign, &rchunk) >= 0) {
        int r;
        r = pa_memblockq_push(bq, &rchunk);
        pa_memblock_unref(rchunk.memblock);

        if (r < 0)
            return -1;
    }

    return 0;
}

void pa_memblockq_shorten(pa_memblockq *bq, size_t length) {
    size_t l;
    assert(bq);

    l = pa_memblockq_get_length(bq);

    if (l > length)
        pa_memblockq_drop(bq, NULL, l - length);
}

void pa_memblockq_prebuf_disable(pa_memblockq *bq) {
    assert(bq);

    if (bq->state == PREBUF)
        bq->state = RUNNING;
}

void pa_memblockq_prebuf_force(pa_memblockq *bq) {
    assert(bq);

    if (bq->state == RUNNING && bq->prebuf > 0)
        bq->state = PREBUF;
}

size_t pa_memblockq_get_maxlength(pa_memblockq *bq) {
    assert(bq);

    return bq->maxlength;
}

size_t pa_memblockq_get_prebuf(pa_memblockq *bq) {
    assert(bq);

    return bq->prebuf;
}
