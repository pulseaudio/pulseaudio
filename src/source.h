#ifndef foosourcehfoo
#define foosourcehfoo

struct pa_source;

#include <inttypes.h>
#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "memblock.h"
#include "memchunk.h"
#include "sink.h"

struct pa_source {
    uint32_t index;
    
    char *name;
    struct pa_core *core;
    struct pa_sample_spec sample_spec;
    struct pa_idxset *outputs;
    struct pa_sink *monitor_of;

    void (*notify)(struct pa_source*source);
    void *userdata;
};

struct pa_source* pa_source_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec);
void pa_source_free(struct pa_source *s);

/* Pass a new memory block to all output streams */
void pa_source_post(struct pa_source*s, struct pa_memchunk *b);

void pa_source_notify(struct pa_source *s);

char *pa_source_list_to_string(struct pa_core *c);

struct pa_source* pa_source_get_default(struct pa_core *c);

#endif
