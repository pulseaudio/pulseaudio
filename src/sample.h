#ifndef foosamplehfoo
#define foosamplehfoo

#include <inttypes.h>
#include <sys/types.h>

enum pa_sample_format {
    PA_SAMPLE_U8,
    PA_SAMPLE_ALAW,
    PA_SAMPLE_ULAW,
    PA_SAMPLE_S16LE,
    PA_SAMPLE_S16BE,
    PA_SAMPLE_FLOAT32,
    PA_SAMPLE_MAX
};

#ifdef WORDS_BIGENDIAN
#define PA_SAMPLE_S16NE PA_SAMPLE_S16BE
#else
#define PA_SAMPLE_S16NE PA_SAMPLE_S16LE
#endif

struct pa_sample_spec {
    enum pa_sample_format format;
    uint32_t rate;
    uint8_t channels;
};

size_t pa_bytes_per_second(const struct pa_sample_spec *spec);
size_t pa_sample_size(const struct pa_sample_spec *spec);
uint32_t pa_samples_usec(size_t length, const struct pa_sample_spec *spec);
int pa_sample_spec_valid(const struct pa_sample_spec *spec);
int pa_sample_spec_equal(const struct pa_sample_spec*a, const struct pa_sample_spec*b);


#define PA_SAMPLE_SNPRINT_MAX_LENGTH 32
void pa_sample_snprint(char *s, size_t l, const struct pa_sample_spec *spec);

#endif
