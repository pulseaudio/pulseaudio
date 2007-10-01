/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/log.h>
#include <pulsecore/mcalign.h>
#include <pulsecore/macro.h>
#include <pulsecore/flist.h>

#include "memblockq.h"

struct list_item {
    struct list_item *next, *prev;
    int64_t index;
    pa_memchunk chunk;
};

PA_STATIC_FLIST_DECLARE(list_items, 0, pa_xfree);

struct pa_memblockq {
    struct list_item *blocks, *blocks_tail;
    unsigned n_blocks;
    size_t maxlength, tlength, base, prebuf, minreq;
    int64_t read_index, write_index;
    pa_bool_t in_prebuf;
    pa_memblock *silence;
    pa_mcalign *mcalign;
    int64_t missing;
    size_t requested;
};

pa_memblockq* pa_memblockq_new(
        int64_t idx,
        size_t maxlength,
        size_t tlength,
        size_t base,
        size_t prebuf,
        size_t minreq,
        pa_memblock *silence) {

    pa_memblockq* bq;

    pa_assert(base > 0);
    pa_assert(maxlength >= base);

    bq = pa_xnew(pa_memblockq, 1);
    bq->blocks = bq->blocks_tail = NULL;
    bq->n_blocks = 0;

    bq->base = base;
    bq->read_index = bq->write_index = idx;

    pa_log_debug("memblockq requested: maxlength=%lu, tlength=%lu, base=%lu, prebuf=%lu, minreq=%lu",
        (unsigned long) maxlength, (unsigned long) tlength, (unsigned long) base, (unsigned long) prebuf, (unsigned long) minreq);

    bq->maxlength = ((maxlength+base-1)/base)*base;
    pa_assert(bq->maxlength >= base);

    bq->tlength = ((tlength+base-1)/base)*base;
    if (bq->tlength <= 0 || bq->tlength > bq->maxlength)
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

    pa_log_debug("memblockq sanitized: maxlength=%lu, tlength=%lu, base=%lu, prebuf=%lu, minreq=%lu",
        (unsigned long)bq->maxlength, (unsigned long)bq->tlength, (unsigned long)bq->base, (unsigned long)bq->prebuf, (unsigned long)bq->minreq);

    bq->in_prebuf = bq->prebuf > 0;
    bq->silence = silence ? pa_memblock_ref(silence) : NULL;
    bq->mcalign = NULL;

    bq->missing = bq->tlength;
    bq->requested = 0;

    return bq;
}

void pa_memblockq_free(pa_memblockq* bq) {
    pa_assert(bq);

    pa_memblockq_flush(bq);

    if (bq->silence)
        pa_memblock_unref(bq->silence);

    if (bq->mcalign)
        pa_mcalign_free(bq->mcalign);

    pa_xfree(bq);
}

static void drop_block(pa_memblockq *bq, struct list_item *q) {
    pa_assert(bq);
    pa_assert(q);

    pa_assert(bq->n_blocks >= 1);

    if (q->prev)
        q->prev->next = q->next;
    else
        bq->blocks = q->next;

    if (q->next)
        q->next->prev = q->prev;
    else
        bq->blocks_tail = q->prev;

    pa_memblock_unref(q->chunk.memblock);

    if (pa_flist_push(PA_STATIC_FLIST_GET(list_items), q) < 0)
        pa_xfree(q);

    bq->n_blocks--;
}

static pa_bool_t can_push(pa_memblockq *bq, size_t l) {
    int64_t end;

    pa_assert(bq);

    if (bq->read_index > bq->write_index) {
        size_t d =  bq->read_index - bq->write_index;

        if (l > d)
            l -= d;
        else
            return TRUE;
    }

    end = bq->blocks_tail ? bq->blocks_tail->index + bq->blocks_tail->chunk.length : 0;

    /* Make sure that the list doesn't get too long */
    if (bq->write_index + (int64_t)l > end)
        if (bq->write_index + l - bq->read_index > bq->maxlength)
            return FALSE;

    return TRUE;
}

int pa_memblockq_push(pa_memblockq* bq, const pa_memchunk *uchunk) {
    struct list_item *q, *n;
    pa_memchunk chunk;
    int64_t old, delta;

    pa_assert(bq);
    pa_assert(uchunk);
    pa_assert(uchunk->memblock);
    pa_assert(uchunk->length > 0);
    pa_assert(uchunk->index + uchunk->length <= pa_memblock_get_length(uchunk->memblock));

    if (uchunk->length % bq->base)
        return -1;

    if (!can_push(bq, uchunk->length))
        return -1;

    old = bq->write_index;
    chunk = *uchunk;

    if (bq->read_index > bq->write_index) {

        /* We currently have a buffer underflow, we need to drop some
         * incoming data */

        size_t d = bq->read_index - bq->write_index;

        if (chunk.length > d) {
            chunk.index += d;
            chunk.length -= d;
            bq->write_index += d;
        } else {
            /* We drop the incoming data completely */
            bq->write_index += chunk.length;
            goto finish;
        }
    }

    /* We go from back to front to look for the right place to add
     * this new entry. Drop data we will overwrite on the way */

    q = bq->blocks_tail;
    while (q) {

        if (bq->write_index >= q->index + (int64_t) q->chunk.length)
            /* We found the entry where we need to place the new entry immediately after */
            break;
        else if (bq->write_index + (int64_t) chunk.length <= q->index) {
            /* This entry isn't touched at all, let's skip it */
            q = q->prev;
        } else if (bq->write_index <= q->index &&
            bq->write_index + chunk.length >= q->index + q->chunk.length) {

            /* This entry is fully replaced by the new entry, so let's drop it */

            struct list_item *p;
            p = q;
            q = q->prev;
            drop_block(bq, p);
        } else if (bq->write_index >= q->index) {
            /* The write index points into this memblock, so let's
             * truncate or split it */

            if (bq->write_index + chunk.length < q->index + q->chunk.length) {

                /* We need to save the end of this memchunk */
                struct list_item *p;
                size_t d;

                /* Create a new list entry for the end of thie memchunk */
                if (!(p = pa_flist_pop(PA_STATIC_FLIST_GET(list_items))))
                    p = pa_xnew(struct list_item, 1);

                p->chunk = q->chunk;
                pa_memblock_ref(p->chunk.memblock);

                /* Calculate offset */
                d = bq->write_index + chunk.length - q->index;
                pa_assert(d > 0);

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
                struct list_item *p;
                p = q;
                q = q->prev;
                drop_block(bq, p);
            }

            /* We had to truncate this block, hence we're now at the right position */
            break;
        } else {
            size_t d;

            pa_assert(bq->write_index + (int64_t)chunk.length > q->index &&
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
        pa_assert(bq->write_index >=  q->index + (int64_t)q->chunk.length);
        pa_assert(!q->next || (bq->write_index + (int64_t)chunk.length <= q->next->index));

        /* Try to merge memory blocks */

        if (q->chunk.memblock == chunk.memblock &&
            q->chunk.index + (int64_t)q->chunk.length == chunk.index &&
            bq->write_index == q->index + (int64_t)q->chunk.length) {

            q->chunk.length += chunk.length;
            bq->write_index += chunk.length;
            goto finish;
        }
    } else
        pa_assert(!bq->blocks || (bq->write_index + (int64_t)chunk.length <= bq->blocks->index));

    if (!(n = pa_flist_pop(PA_STATIC_FLIST_GET(list_items))))
        n = pa_xnew(struct list_item, 1);

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

finish:

    delta = bq->write_index - old;

    if (delta >= bq->requested) {
        delta -= bq->requested;
        bq->requested = 0;
    } else {
        bq->requested -= delta;
        delta = 0;
    }

    bq->missing -= delta;

    return 0;
}

static pa_bool_t memblockq_check_prebuf(pa_memblockq *bq) {
    pa_assert(bq);

    if (bq->in_prebuf) {

        if (pa_memblockq_get_length(bq) < bq->prebuf)
            return TRUE;

        bq->in_prebuf = FALSE;
        return FALSE;
    } else {

        if (bq->prebuf > 0 && bq->read_index >= bq->write_index) {
            bq->in_prebuf = TRUE;
            return TRUE;
        }

        return FALSE;
    }
}

int pa_memblockq_peek(pa_memblockq* bq, pa_memchunk *chunk) {
    pa_assert(bq);
    pa_assert(chunk);

    /* We need to pre-buffer */
    if (memblockq_check_prebuf(bq))
        return -1;

    /* Do we need to spit out silence? */
    if (!bq->blocks || bq->blocks->index > bq->read_index) {

        size_t length;

        /* How much silence shall we return? */
        length = bq->blocks ? bq->blocks->index - bq->read_index : 0;

        /* We need to return silence, since no data is yet available */
        if (bq->silence) {
            chunk->memblock = pa_memblock_ref(bq->silence);

            if (!length || length > pa_memblock_get_length(chunk->memblock))
                length = pa_memblock_get_length(chunk->memblock);

            chunk->length = length;
        } else {

            /* If the memblockq is empty, return -1, otherwise return
             * the time to sleep */
            if (!bq->blocks)
                return -1;

            chunk->memblock = NULL;
            chunk->length = length;
        }

        chunk->index = 0;
        return 0;
    }

    /* Ok, let's pass real data to the caller */
    pa_assert(bq->blocks->index == bq->read_index);

    *chunk = bq->blocks->chunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

void pa_memblockq_drop(pa_memblockq *bq, size_t length) {
    int64_t old, delta;
    pa_assert(bq);
    pa_assert(length % bq->base == 0);

    old = bq->read_index;

    while (length > 0) {

        /* Do not drop any data when we are in prebuffering mode */
        if (memblockq_check_prebuf(bq))
            break;

        if (bq->blocks) {
            size_t d;

            pa_assert(bq->blocks->index >= bq->read_index);

            d = (size_t) (bq->blocks->index - bq->read_index);

            if (d >= length) {
                /* The first block is too far in the future */

                bq->read_index += length;
                break;
            } else {

                length -= d;
                bq->read_index += d;
            }

            pa_assert(bq->blocks->index == bq->read_index);

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

    delta = bq->read_index - old;
    bq->missing += delta;
}

int pa_memblockq_is_readable(pa_memblockq *bq) {
    pa_assert(bq);

    if (memblockq_check_prebuf(bq))
        return 0;

    if (pa_memblockq_get_length(bq) <= 0)
        return 0;

    return 1;
}

size_t pa_memblockq_get_length(pa_memblockq *bq) {
    pa_assert(bq);

    if (bq->write_index <= bq->read_index)
        return 0;

    return (size_t) (bq->write_index - bq->read_index);
}

size_t pa_memblockq_missing(pa_memblockq *bq) {
    size_t l;
    pa_assert(bq);

    if ((l = pa_memblockq_get_length(bq)) >= bq->tlength)
        return 0;

    l = bq->tlength - l;

    return l >= bq->minreq ? l : 0;
}

size_t pa_memblockq_get_minreq(pa_memblockq *bq) {
    pa_assert(bq);

    return bq->minreq;
}

void pa_memblockq_seek(pa_memblockq *bq, int64_t offset, pa_seek_mode_t seek) {
    int64_t old, delta;
    pa_assert(bq);

    old = bq->write_index;

    switch (seek) {
        case PA_SEEK_RELATIVE:
            bq->write_index += offset;
            break;
        case PA_SEEK_ABSOLUTE:
            bq->write_index = offset;
            break;
        case PA_SEEK_RELATIVE_ON_READ:
            bq->write_index = bq->read_index + offset;
            break;
        case PA_SEEK_RELATIVE_END:
            bq->write_index = (bq->blocks_tail ? bq->blocks_tail->index + (int64_t) bq->blocks_tail->chunk.length : bq->read_index) + offset;
            break;
        default:
            pa_assert_not_reached();
    }

    delta = bq->write_index - old;

    if (delta >= bq->requested) {
        delta -= bq->requested;
        bq->requested = 0;
    } else if (delta >= 0) {
        bq->requested -= delta;
        delta = 0;
    }

    bq->missing -= delta;
}

void pa_memblockq_flush(pa_memblockq *bq) {
    int64_t old, delta;
    pa_assert(bq);

    while (bq->blocks)
        drop_block(bq, bq->blocks);

    pa_assert(bq->n_blocks == 0);

    old = bq->write_index;
    bq->write_index = bq->read_index;

    pa_memblockq_prebuf_force(bq);

    delta = bq->write_index - old;

    if (delta > bq->requested) {
        delta -= bq->requested;
        bq->requested = 0;
    } else if (delta >= 0) {
        bq->requested -= delta;
        delta = 0;
    }

    bq->missing -= delta;
}

size_t pa_memblockq_get_tlength(pa_memblockq *bq) {
    pa_assert(bq);

    return bq->tlength;
}

int64_t pa_memblockq_get_read_index(pa_memblockq *bq) {
    pa_assert(bq);
    return bq->read_index;
}

int64_t pa_memblockq_get_write_index(pa_memblockq *bq) {
    pa_assert(bq);
    return bq->write_index;
}

int pa_memblockq_push_align(pa_memblockq* bq, const pa_memchunk *chunk) {
    pa_memchunk rchunk;

    pa_assert(bq);
    pa_assert(chunk);

    if (bq->base == 1)
        return pa_memblockq_push(bq, chunk);

    if (!bq->mcalign)
        bq->mcalign = pa_mcalign_new(bq->base);

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
    pa_assert(bq);

    l = pa_memblockq_get_length(bq);

    if (l > length)
        pa_memblockq_drop(bq, l - length);
}

void pa_memblockq_prebuf_disable(pa_memblockq *bq) {
    pa_assert(bq);

    bq->in_prebuf = FALSE;
}

void pa_memblockq_prebuf_force(pa_memblockq *bq) {
    pa_assert(bq);

    if (!bq->in_prebuf && bq->prebuf > 0)
        bq->in_prebuf = TRUE;
}

size_t pa_memblockq_get_maxlength(pa_memblockq *bq) {
    pa_assert(bq);

    return bq->maxlength;
}

size_t pa_memblockq_get_prebuf(pa_memblockq *bq) {
    pa_assert(bq);

    return bq->prebuf;
}

size_t pa_memblockq_pop_missing(pa_memblockq *bq) {
    size_t l;

    pa_assert(bq);

/*     pa_log("pop: %lli", bq->missing); */

    if (bq->missing <= 0)
        return 0;

    l = (size_t) bq->missing;
    bq->missing = 0;
    bq->requested += l;

    return l;
}
