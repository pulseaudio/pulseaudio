#ifndef foosinkinputhfoo
#define foosinkinputhfoo

#include <inttypes.h>

#include "sink.h"
#include "sample.h"
#include "memblockq.h"
#include "resampler.h"
#include "module.h"
#include "client.h"

struct pa_sink_input {
    uint32_t index;

    char *name;
    struct pa_module *owner;
    struct pa_client *client;
    struct pa_sink *sink;
    struct pa_sample_spec sample_spec;
    uint32_t volume;
    
    int (*peek) (struct pa_sink_input *i, struct pa_memchunk *chunk);
    void (*drop) (struct pa_sink_input *i, size_t length);
    void (*kill) (struct pa_sink_input *i);
    uint32_t (*get_latency) (struct pa_sink_input *i);

    void *userdata;

    struct pa_memchunk resampled_chunk;
    struct pa_resampler *resampler;
};

struct pa_sink_input* pa_sink_input_new(struct pa_sink *s, const char *name, const struct pa_sample_spec *spec);
void pa_sink_input_free(struct pa_sink_input* i);

/* Code that didn't create the input stream should call this function to
 * request destruction of it */
void pa_sink_input_kill(struct pa_sink_input *i);

uint32_t pa_sink_input_get_latency(struct pa_sink_input *i);
char *pa_sink_input_list_to_string(struct pa_core *c);

int pa_sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk);
void pa_sink_input_drop(struct pa_sink_input *i, size_t length);

#endif
