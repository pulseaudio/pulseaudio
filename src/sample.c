#include <string.h>
#include <assert.h>

#include "sample.h"

struct sample_spec default_sample_spec = {
    .format = SAMPLE_S16NE,
    .rate = 44100,
    .channels = 2
};

struct memblock *silence(struct memblock* b, struct sample_spec *spec) {
    char c = 0;
    assert(b && spec);
    memblock_assert_exclusive(b);

    switch (spec->format) {
        case SAMPLE_U8:
            c = 127;
            break;
        case SAMPLE_S16LE:
        case SAMPLE_S16BE:
        case SAMPLE_FLOAT32:
            c = 0;
            break;
        case SAMPLE_ALAW:
        case SAMPLE_ULAW:
            c = 80;
            break;
    }
                
    memset(b->data, c, b->length);
    return b;
}

void add_clip(struct memchunk *target, struct memchunk *chunk, struct sample_spec *spec) {
    int16_t *p, *d;
    size_t i;
    assert(target && target->memblock && chunk && chunk->memblock && spec);
    assert(spec->format == SAMPLE_S16NE);
    assert((target->length & 1) == 0);
    
    d = target->memblock->data + target->index;
    p = chunk->memblock->data + chunk->index;

    for (i = 0; i < target->length && i < chunk->length; i++) {
        int32_t r = (int32_t) *d + (int32_t) *p;
        if (r < -0x8000) r = 0x8000;
        if (r > 0x7FFF) r = 0x7FFF;
        *d = (int16_t) r;
    }
}

size_t sample_size(struct sample_spec *spec) {
    assert(spec);
    size_t b = 1;

    switch (spec->format) {
        case SAMPLE_U8:
        case SAMPLE_ULAW:
        case SAMPLE_ALAW:
            b = 1;
            break;
        case SAMPLE_S16LE:
        case SAMPLE_S16BE:
            b = 2;
            break;
        case SAMPLE_FLOAT32:
            b = 4;
            break;
    }

    return b * spec->channels;
}

size_t bytes_per_second(struct sample_spec *spec) {
    assert(spec);
    return spec->rate*sample_size(spec);
}

