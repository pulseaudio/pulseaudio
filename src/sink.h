#ifndef foosinkhfoo
#define foosinkhfoo

struct pa_sink;

#include <inttypes.h>

#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "source.h"

struct pa_sink {
    uint32_t index;

    char *name, *description;
    struct pa_module *owner;
    struct pa_core *core;
    struct pa_sample_spec sample_spec;
    struct pa_idxset *inputs;

    struct pa_source *monitor_source;

    uint32_t volume;

    void (*notify)(struct pa_sink*sink);
    uint32_t (*get_latency)(struct pa_sink *s);
    void *userdata;
};

struct pa_sink* pa_sink_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec);
void pa_sink_free(struct pa_sink* s);

int pa_sink_render(struct pa_sink*s, size_t length, struct pa_memchunk *result);
int pa_sink_render_into(struct pa_sink*s, struct pa_memchunk *target);
void pa_sink_render_into_full(struct pa_sink *s, struct pa_memchunk *target);

uint32_t pa_sink_get_latency(struct pa_sink *s);

void pa_sink_notify(struct pa_sink*s);

struct pa_sink* pa_sink_get_default(struct pa_core *c);

void pa_sink_set_owner(struct pa_sink *sink, struct pa_module *m);

#endif
