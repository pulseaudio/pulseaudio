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

size_t mix_chunks(struct mix_info channels[], unsigned nchannels, void *data, size_t length, struct sample_spec *spec) {
    unsigned c, d;
    assert(chunks && target && spec);
    assert(spec->format == SAMPLE_S16NE);

    for (d = 0;; d += sizeof(int16_t)) {
        int32_t sum = 0;

        if (d >= length)
            return d;
        
        for (c = 0; c < nchannels; c++) {
            int32_t v;
            uint8_t volume = channels[c].volume;
            
            if (d >= channels[c].chunk.length)
                return d;

            if (volume == 0)
                v = 0;
            else {
                v = *((int16_t*) (channels[c].chunk->memblock->data + channels[c].chunk->index + d));

                if (volume != 0xFF)
                    v = v*volume/0xFF;
            }

            sum += v;
        }

        if (sum < -0x8000) sum = -0x8000;
        if (sum > 0x7FFF) sum = 0x7FFF;
        *(data++) = sum;
    }
}
