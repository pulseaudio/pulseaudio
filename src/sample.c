#include <assert.h>

#include "sample.h"

size_t pa_sample_size(struct pa_sample_spec *spec) {
    assert(spec);
    size_t b = 1;

    switch (spec->format) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW:
            b = 1;
            break;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            b = 2;
            break;
        case PA_SAMPLE_FLOAT32:
            b = 4;
            break;
        default:
            assert(0);
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

int pa_sample_spec_valid(struct pa_sample_spec *spec) {
    assert(spec);

    if (!spec->rate || !spec->channels)
        return 0;

    if (spec->format <= 0 || spec->format >= PA_SAMPLE_MAX)
        return 0;

    return 1;
}
