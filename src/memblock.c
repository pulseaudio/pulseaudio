#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "memblock.h"

unsigned n_blocks = 0;

struct memblock *memblock_new(size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock)+length);
    b->type = MEMBLOCK_APPENDED;
    b->ref = 1;
    b->length = length;
    b->data = b+1;
    n_blocks++;
    timerclear(&b->stamp);
    return b;
}

struct memblock *memblock_new_fixed(void *d, size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock));
    b->type = MEMBLOCK_FIXED;
    b->ref = 1;
    b->length = length;
    b->data = d;
    n_blocks++;
    timerclear(&b->stamp);
    return b;
}

struct memblock *memblock_new_dynamic(void *d, size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock));
    b->type = MEMBLOCK_DYNAMIC;
    b->ref = 1;
    b->length = length;
    b->data = d;
    n_blocks++;
    timerclear(&b->stamp);
    return b;
}

struct memblock* memblock_ref(struct memblock*b) {
    assert(b && b->ref >= 1);
    b->ref++;
    return b;
}

void memblock_unref(struct memblock*b) {
    assert(b && b->ref >= 1);
    b->ref--;

    if (b->ref == 0) {
        if (b->type == MEMBLOCK_DYNAMIC)
            free(b->data);
        free(b);
        n_blocks--;
    }
}

void memblock_unref_fixed(struct memblock *b) {
    void *d;
    
    assert(b && b->ref >= 1);

    if (b->ref == 1) {
        memblock_unref(b);
        return;
    }

    d = malloc(b->length);
    assert(d);
    memcpy(d, b->data, b->length);
    b->data = d;
    b->type = MEMBLOCK_DYNAMIC;
}

void memblock_stamp(struct memblock*b) {
    assert(b);
    gettimeofday(&b->stamp, NULL);
}

uint32_t memblock_age(struct memblock*b) {
    assert(b);
    struct timeval tv;
    uint32_t r;

    if (b->stamp.tv_sec == 0)
        return (suseconds_t) -1;

    gettimeofday(&tv, NULL);

    /*fprintf(stderr, "memblock: (%lu,%lu) -- (%lu,%lu)\r", b->stamp.tv_sec, b->stamp.tv_usec, tv.tv_sec, tv.tv_usec);*/
    
    r = (tv.tv_sec-b->stamp.tv_sec) * 1000000;

    if (tv.tv_usec >= b->stamp.tv_usec)
        r += tv.tv_usec - b->stamp.tv_usec;
    else
        r -= b->stamp.tv_usec - tv.tv_usec;

    return r;
}
