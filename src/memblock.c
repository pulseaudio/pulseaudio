#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "memblock.h"

unsigned pa_memblock_count = 0, pa_memblock_total = 0;

struct pa_memblock *pa_memblock_new(size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock)+length);
    b->type = PA_MEMBLOCK_APPENDED;
    b->ref = 1;
    b->length = length;
    b->data = b+1;
    pa_memblock_count++;
    pa_memblock_total += length;
    return b;
}

struct pa_memblock *pa_memblock_new_fixed(void *d, size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_FIXED;
    b->ref = 1;
    b->length = length;
    b->data = d;
    pa_memblock_count++;
    pa_memblock_total += length;
    return b;
}

struct pa_memblock *pa_memblock_new_dynamic(void *d, size_t length) {
    struct pa_memblock *b = malloc(sizeof(struct pa_memblock));
    b->type = PA_MEMBLOCK_DYNAMIC;
    b->ref = 1;
    b->length = length;
    b->data = d;
    pa_memblock_count++;
    pa_memblock_total += length;
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

        pa_memblock_count--;
        pa_memblock_total -= b->length;

        free(b);
    }
}

void pa_memblock_unref_fixed(struct pa_memblock *b) {
    void *d;
    
    assert(b && b->ref >= 1);

    if (b->ref == 1) {
        pa_memblock_unref(b);
        return;
    }

    d = malloc(b->length);
    assert(d);
    memcpy(d, b->data, b->length);
    b->data = d;
    b->type = PA_MEMBLOCK_DYNAMIC;
}

