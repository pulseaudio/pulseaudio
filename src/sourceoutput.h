#ifndef foosourceoutputhfoo
#define foosourceoutputhfoo

#include <inttypes.h>

#include "source.h"
#include "sample.h"
#include "memblockq.h"
#include "resampler.h"
#include "module.h"
#include "client.h"

struct pa_source_output {
    uint32_t index;

    char *name;
    struct pa_module *owner;
    struct pa_client *client;
    struct pa_source *source;
    struct pa_sample_spec sample_spec;
    
    void (*push)(struct pa_source_output *o, const struct pa_memchunk *chunk);
    void (*kill)(struct pa_source_output* o);

    struct pa_resampler* resampler;
    
    void *userdata;
};

struct pa_source_output* pa_source_output_new(struct pa_source *s, const char *name, const struct pa_sample_spec *spec);
void pa_source_output_free(struct pa_source_output* o);

void pa_source_output_kill(struct pa_source_output*o);

char *pa_source_output_list_to_string(struct pa_core *c);

void pa_source_output_push(struct pa_source_output *o, const struct pa_memchunk *chunk);

#endif
