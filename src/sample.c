#include <stdio.h>
#include <assert.h>

#include "sample.h"

size_t pa_sample_size(const struct pa_sample_spec *spec) {
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

size_t pa_bytes_per_second(const struct pa_sample_spec *spec) {
    assert(spec);
    return spec->rate*pa_sample_size(spec);
}


uint32_t pa_samples_usec(size_t length, const struct pa_sample_spec *spec) {
    assert(spec);

    return (uint32_t) (((double) length /pa_sample_size(spec))/spec->rate*1000000);
}

int pa_sample_spec_valid(const struct pa_sample_spec *spec) {
    assert(spec);

    if (!spec->rate || !spec->channels)
        return 0;

    if (spec->format >= PA_SAMPLE_MAX)
        return 0;

    return 1;
}

int pa_sample_spec_equal(const struct pa_sample_spec*a, const struct pa_sample_spec*b) {
    assert(a && b);

    return (a->format == b->format) && (a->rate == b->rate) && (a->channels == b->channels);
}

void pa_sample_snprint(char *s, size_t l, const struct pa_sample_spec *spec) {
    static const char* const table[]= {
        [PA_SAMPLE_U8] = "U8",
        [PA_SAMPLE_ALAW] = "ALAW",
        [PA_SAMPLE_ULAW] = "ULAW",
        [PA_SAMPLE_S16LE] = "S16LE",
        [PA_SAMPLE_S16BE] = "S16BE",
        [PA_SAMPLE_FLOAT32] = "FLOAT32",
    };

    assert(pa_sample_spec_valid(spec));
    snprintf(s, l, "%s %uch %uHz", table[spec->format], spec->channels, spec->rate);
}
