#include <string.h>
#include <assert.h>

#include "sample-util.h"

struct pa_sample_spec default_sample_spec = {
    .format = PA_SAMPLE_S16NE,
    .rate = 44100,
    .channels = 2
};

struct memblock *silence_memblock(struct memblock* b, struct pa_sample_spec *spec) {
    assert(b && b->data && spec);
    memblock_assert_exclusive(b);
    silence_memory(b->data, b->length, spec);
    return b;
}

void silence_memchunk(struct memchunk *c, struct pa_sample_spec *spec) {
    assert(c && c->memblock && c->memblock->data && spec && c->length);
    memblock_assert_exclusive(c->memblock);
    silence_memory(c->memblock->data+c->index, c->length, spec);
}

void silence_memory(void *p, size_t length, struct pa_sample_spec *spec) {
    char c = 0;
    assert(p && length && spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
            c = 127;
            break;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_FLOAT32:
            c = 0;
            break;
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_ULAW:
            c = 80;
            break;
        default:
            assert(0);
    }
                
    memset(p, c, length);
}

size_t mix_chunks(struct mix_info channels[], unsigned nchannels, void *data, size_t length, struct pa_sample_spec *spec, uint8_t volume) {
    unsigned c, d;
    assert(channels && data && length && spec);
    assert(spec->format == PA_SAMPLE_S16NE);

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
                v = *((int16_t*) (channels[c].chunk.memblock->data + channels[c].chunk.index + d));

                if (volume != 0xFF)
                    v = v*volume/0xFF;
            }

            sum += v;
        }

        if (volume == 0)
            sum = 0;
        else if (volume != 0xFF)
            sum = sum*volume/0xFF;
        
        if (sum < -0x8000) sum = -0x8000;
        if (sum > 0x7FFF) sum = 0x7FFF;
        
        *((int16_t*) data) = sum;
        data += sizeof(int16_t);
    }
}
