#ifndef foosinkinputhfoo
#define foosinkinputhfoo

#include <inttypes.h>

#include "sink.h"
#include "sample.h"
#include "memblockq.h"

struct sink_input {
    uint32_t index;

    char *name;
    struct sink *sink;
    struct pa_sample_spec sample_spec;
    uint8_t volume;
    
    int (*peek) (struct sink_input *i, struct memchunk *chunk);
    void (*drop) (struct sink_input *i, size_t length);
    void (*kill) (struct sink_input *i);
    uint32_t (*get_latency) (struct sink_input *i);

    void *userdata;
};

struct sink_input* sink_input_new(struct sink *s, struct pa_sample_spec *spec, const char *name);
void sink_input_free(struct sink_input* i);

/* Code that didn't create the input stream should call this function to
 * request destruction of it */
void sink_input_kill(struct sink_input *i);

uint32_t sink_input_get_latency(struct sink_input *i);
char *sink_input_list_to_string(struct core *c);

#endif
