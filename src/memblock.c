#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "memblock.h"

unsigned memblock_count = 0, memblock_total = 0;

struct memblock *memblock_new(size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock)+length);
    b->type = MEMBLOCK_APPENDED;
    b->ref = 1;
    b->length = length;
    b->data = b+1;
    memblock_count++;
    memblock_total += length;
    return b;
}

struct memblock *memblock_new_fixed(void *d, size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock));
    b->type = MEMBLOCK_FIXED;
    b->ref = 1;
    b->length = length;
    b->data = d;
    memblock_count++;
    memblock_total += length;
    return b;
}

struct memblock *memblock_new_dynamic(void *d, size_t length) {
    struct memblock *b = malloc(sizeof(struct memblock));
    b->type = MEMBLOCK_DYNAMIC;
    b->ref = 1;
    b->length = length;
    b->data = d;
    memblock_count++;
    memblock_total += length;
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

        memblock_count--;
        memblock_total -= b->length;

        free(b);
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

