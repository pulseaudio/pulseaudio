#ifndef foosampleutilhfoo
#define foosampleutilhfoo

#include "sample.h"
#include "memblock.h"
#include "memchunk.h"

#define PA_VOLUME_NORM (0x100)
#define PA_VOLUME_MUTE (0)

struct pa_memblock *pa_silence_memblock(struct pa_memblock* b, const struct pa_sample_spec *spec);
void pa_silence_memchunk(struct pa_memchunk *c, const struct pa_sample_spec *spec);
void pa_silence_memory(void *p, size_t length, const struct pa_sample_spec *spec);

struct pa_mix_info {
    struct pa_memchunk chunk;
    uint32_t volume;
    void *userdata;
};

size_t pa_mix(struct pa_mix_info channels[], unsigned nchannels, void *data, size_t length, const struct pa_sample_spec *spec, uint32_t volume);

void pa_volume_memchunk(struct pa_memchunk*c, const struct pa_sample_spec *spec, uint32_t volume);

uint32_t pa_volume_multiply(uint32_t a, uint32_t b);

#endif
