#include <assert.h>

#include "sample.h"

size_t pa_sample_size(struct pa_sample_spec *spec) {
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

size_t pa_bytes_per_second(struct pa_sample_spec *spec) {
    assert(spec);
    return spec->rate*pa_sample_size(spec);
}


uint32_t pa_samples_usec(size_t length, struct pa_sample_spec *spec) {
    assert(spec);

    return (uint32_t) (((double) length /pa_sample_size(spec))/spec->rate*1000000);
}
