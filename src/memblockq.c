#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include "memblockq.h"

struct memblock_list {
    struct memblock_list *next;
    struct memchunk chunk;
    struct timeval stamp;
};

struct memblockq {
    struct memblock_list *blocks, *blocks_tail;
    unsigned n_blocks;
    size_t total_length, maxlength, base, prebuf;
    int measure_delay;
    uint32_t delay;
};

struct memblockq* memblockq_new(size_t maxlength, size_t base, size_t prebuf) {
    struct memblockq* bq;
    assert(maxlength && base);
    
    bq = malloc(sizeof(struct memblockq));
    assert(bq);
    bq->blocks = bq->blocks_tail = 0;
    bq->n_blocks = 0;
    bq->total_length = 0;
    bq->base = base;
    bq->maxlength = ((maxlength+base-1)/base)*base;
    bq->prebuf = prebuf == (size_t) -1 ? bq->maxlength/2 : prebuf;
    
    if (bq->prebuf > bq->maxlength)
        bq->prebuf = bq->maxlength;
    
    assert(bq->maxlength >= base);

    bq->measure_delay = 0;
    bq->delay = 0;
    
    return bq;
}

void memblockq_free(struct memblockq* bq) {
    struct memblock_list *l;
    assert(bq);

    while ((l = bq->blocks)) {
        bq->blocks = l->next;
        memblock_unref(l->chunk.memblock);
        free(l);
    }
    
    free(bq);
}

void memblockq_push(struct memblockq* bq, struct memchunk *chunk, size_t delta) {
    struct memblock_list *q;
    assert(bq && chunk && chunk->memblock && chunk->length);

    q = malloc(sizeof(struct memblock_list));
    assert(q);

    if (bq->measure_delay)
        gettimeofday(&q->stamp, NULL);
    else
        timerclear(&q->stamp);

    q->chunk = *chunk;
    memblock_ref(q->chunk.memblock);
    assert(q->chunk.index+q->chunk.length <= q->chunk.memblock->length);
    q->next = NULL;
    
    if (bq->blocks_tail)
        bq->blocks_tail->next = q;
    else
        bq->blocks = q;
    
    bq->blocks_tail = q;

    bq->n_blocks++;
    bq->total_length += chunk->length;

    memblockq_shorten(bq, bq->maxlength);
}

int memblockq_peek(struct memblockq* bq, struct memchunk *chunk) {
    assert(bq && chunk);

    if (!bq->blocks || bq->total_length < bq->prebuf)
        return -1;

    bq->prebuf = 0;

    *chunk = bq->blocks->chunk;
    memblock_ref(chunk->memblock);
    return 0;
}

int memblockq_pop(struct memblockq* bq, struct memchunk *chunk) {
    struct memblock_list *q;
    
    assert(bq && chunk);

    if (!bq->blocks || bq->total_length < bq->prebuf)
        return -1;

    bq->prebuf = 0;

    q = bq->blocks;
    bq->blocks = bq->blocks->next;

    *chunk = q->chunk;

    bq->n_blocks--;
    bq->total_length -= chunk->length;

    free(q);
    return 0;
}

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

void memblockq_drop(struct memblockq *bq, size_t length) {
    assert(bq);

    while (length > 0) {
        size_t l = length;
        assert(bq->blocks && bq->total_length >= length);
        
        if (l > bq->blocks->chunk.length)
            l = bq->blocks->chunk.length;

        if (bq->measure_delay)
            bq->delay = age(&bq->blocks->stamp);
        
        bq->blocks->chunk.index += l;
        bq->blocks->chunk.length -= l;
        bq->total_length -= l;
        
        if (bq->blocks->chunk.length == 0) {
            struct memblock_list *q;
            
            q = bq->blocks;
            bq->blocks = bq->blocks->next;
            if (bq->blocks == NULL)
                bq->blocks_tail = NULL;
            memblock_unref(q->chunk.memblock);
            free(q);
            
            bq->n_blocks--;
        }

        length -= l;
    }
}

void memblockq_shorten(struct memblockq *bq, size_t length) {
    size_t l;
    assert(bq);

    if (bq->total_length <= length)
        return;

    fprintf(stderr, "Warning! memblockq_shorten()\n");
    
    l = bq->total_length - length;
    l /= bq->base;
    l *= bq->base;

    memblockq_drop(bq, l);
}


void memblockq_empty(struct memblockq *bq) {
    assert(bq);
    memblockq_shorten(bq, 0);
}

int memblockq_is_readable(struct memblockq *bq) {
    assert(bq);

    return bq->total_length >= bq->prebuf;
}

int memblockq_is_writable(struct memblockq *bq, size_t length) {
    assert(bq);

    assert(length <= bq->maxlength);
    return bq->total_length + length <= bq->maxlength;
}

uint32_t memblockq_get_delay(struct memblockq *bq) {
    assert(bq);
    return bq->delay;
}

uint32_t memblockq_get_length(struct memblockq *bq) {
    assert(bq);
    return bq->total_length;
}
