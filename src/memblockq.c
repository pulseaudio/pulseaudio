#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include "memblockq.h"

struct memblock_list {
    struct memblock_list *next;
    struct pa_memchunk chunk;
    struct timeval stamp;
};

struct pa_memblockq {
    struct memblock_list *blocks, *blocks_tail;
    unsigned n_blocks;
    size_t current_length, maxlength, tlength, base, prebuf, minreq;
    int measure_delay;
    uint32_t delay;
    struct pa_mcalign *mcalign;
};

struct pa_memblockq* pa_memblockq_new(size_t maxlength, size_t tlength, size_t base, size_t prebuf, size_t minreq) {
    struct pa_memblockq* bq;
    assert(maxlength && base && maxlength);
    
    bq = malloc(sizeof(struct pa_memblockq));
    assert(bq);
    bq->blocks = bq->blocks_tail = 0;
    bq->n_blocks = 0;

    bq->current_length = 0;

    fprintf(stderr, "memblockq requested: maxlength=%u, tlength=%u, base=%u, prebuf=%u, minreq=%u\n", maxlength, tlength, base, prebuf, minreq);
    
    bq->base = base;

    bq->maxlength = ((maxlength+base-1)/base)*base;
    assert(bq->maxlength >= base);

    bq->tlength = ((tlength+base-1)/base)*base;
    if (bq->tlength == 0 || bq->tlength >= bq->maxlength)
        bq->tlength = bq->maxlength;
    
    bq->prebuf = (prebuf == (size_t) -1) ? bq->maxlength/2 : prebuf;
    bq->prebuf = (bq->prebuf/base)*base;
    if (bq->prebuf > bq->maxlength)
        bq->prebuf = bq->maxlength;
    
    bq->minreq = (minreq/base)*base;
    if (bq->minreq == 0)
        bq->minreq = 1;

    fprintf(stderr, "memblockq sanitized: maxlength=%u, tlength=%u, base=%u, prebuf=%u, minreq=%u\n", bq->maxlength, bq->tlength, bq->base, bq->prebuf, bq->minreq);
    
    bq->measure_delay = 0;
    bq->delay = 0;

    bq->mcalign = NULL;
    
    return bq;
}

void pa_memblockq_free(struct pa_memblockq* bq) {
    struct memblock_list *l;
    assert(bq);

    if (bq->mcalign)
        pa_mcalign_free(bq->mcalign);

    while ((l = bq->blocks)) {
        bq->blocks = l->next;
        pa_memblock_unref(l->chunk.memblock);
        free(l);
    }
    
    free(bq);
}

void pa_memblockq_push(struct pa_memblockq* bq, const struct pa_memchunk *chunk, size_t delta) {
    struct memblock_list *q;
    assert(bq && chunk && chunk->memblock && chunk->length && (chunk->length % bq->base) == 0);

    q = malloc(sizeof(struct memblock_list));
    assert(q);

    if (bq->measure_delay)
        gettimeofday(&q->stamp, NULL);
    else
        timerclear(&q->stamp);

    q->chunk = *chunk;
    pa_memblock_ref(q->chunk.memblock);
    assert(q->chunk.index+q->chunk.length <= q->chunk.memblock->length);
    q->next = NULL;
    
    if (bq->blocks_tail)
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

/*     if (chunk->memblock->ref != 2) */
/*         fprintf(stderr, "block %p with ref %u peeked.\n", chunk->memblock, chunk->memblock->ref); */
    
    return 0;
}

/*
int memblockq_pop(struct memblockq* bq, struct pa_memchunk *chunk) {
    struct memblock_list *q;
    
    assert(bq && chunk);

    if (!bq->blocks || bq->current_length < bq->prebuf)
        return -1;

    bq->prebuf = 0;

    q = bq->blocks;
    bq->blocks = bq->blocks->next;

    *chunk = q->chunk;

    bq->n_blocks--;
    bq->current_length -= chunk->length;

    free(q);
    return 0;
}
*/

static uint32_t age(struct timeval *tv) {
    assert(tv);
    struct timeval now;
    uint32_t r;

    if (tv->tv_sec == 0)
        return 0;

    gettimeofday(&now, NULL);
    
    r = (now.tv_sec-tv->tv_sec) * 1000000;

    if (now.tv_usec >= tv->tv_usec)
        r += now.tv_usec - tv->tv_usec;
    else
        r -= tv->tv_usec - now.tv_usec;

    return r;
}

void pa_memblockq_drop(struct pa_memblockq *bq, size_t length) {
    assert(bq && length && (length % bq->base) == 0);

    while (length > 0) {
        size_t l = length;
        assert(bq->blocks && bq->current_length >= length);
        
        if (l > bq->blocks->chunk.length)
            l = bq->blocks->chunk.length;

        if (bq->measure_delay)
            bq->delay = age(&bq->blocks->stamp);
        
        bq->blocks->chunk.index += l;
        bq->blocks->chunk.length -= l;
        bq->current_length -= l;
        
        if (bq->blocks->chunk.length == 0) {
            struct memblock_list *q;
            
            q = bq->blocks;
            bq->blocks = bq->blocks->next;
            if (bq->blocks == NULL)
                bq->blocks_tail = NULL;
            pa_memblock_unref(q->chunk.memblock);
            free(q);
            
            bq->n_blocks--;
        }

        length -= l;
    }
}

void pa_memblockq_shorten(struct pa_memblockq *bq, size_t length) {
    size_t l;
    assert(bq);

    if (bq->current_length <= length)
        return;

    fprintf(stderr, "Warning! pa_memblockq_shorten()\n");
    
    l = bq->current_length - length;
    l /= bq->base;
    l *= bq->base;

    pa_memblockq_drop(bq, l);
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

uint32_t pa_memblockq_get_delay(struct pa_memblockq *bq) {
    assert(bq);
    return bq->delay;
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
        bq->mcalign = pa_mcalign_new(bq->base);
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
