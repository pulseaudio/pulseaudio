#ifndef foosamplehfoo
#define foosamplehfoo

#include <inttypes.h>

#include "memblock.h"

enum sample_format {
    SAMPLE_U8,
    SAMPLE_ALAW,
    SAMPLE_ULAW,
    SAMPLE_S16LE,
    SAMPLE_S16BE,
    SAMPLE_FLOAT32
};

#define SAMPLE_S16NE SAMPLE_S16LE

struct sample_spec {
    enum sample_format format;
    uint32_t rate;
    uint32_t channels;
};

#define DEFAULT_SAMPLE_SPEC default_sample_spec

extern struct sample_spec default_sample_spec;

struct memblock *silence(struct memblock* b, struct sample_spec *spec);


struct mix_info {
    struct memchunk chunk;
    uint8_t volume;
    void *userdata;
};

size_t mix_chunks(struct mix_info channels[], unsigned nchannels, void *data, size_t length, struct sample_spec *spec) {

size_t bytes_per_second(struct sample_spec *spec);
size_t sample_size(struct sample_spec *spec);

#endif
