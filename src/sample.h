#ifndef foosamplehfoo
#define foosamplehfoo

#include <inttypes.h>
#include <sys/types.h>

enum pa_sample_format {
    SAMPLE_U8,
    SAMPLE_ALAW,
    SAMPLE_ULAW,
    SAMPLE_S16LE,
    SAMPLE_S16BE,
    SAMPLE_FLOAT32
};

#define SAMPLE_S16NE SAMPLE_S16LE

struct pa_sample_spec {
    enum pa_sample_format format;
    uint32_t rate;
    uint8_t channels;
};

size_t pa_bytes_per_second(struct pa_sample_spec *spec);
size_t pa_sample_size(struct pa_sample_spec *spec);
uint32_t pa_samples_usec(size_t length, struct pa_sample_spec *spec);

#endif
