#ifndef foosinkhfoo
#define foosinkhfoo

struct sink;

#include <inttypes.h>

#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "source.h"

struct sink {
    uint32_t index;

    char *name;
    struct core *core;
    struct pa_sample_spec sample_spec;
    struct idxset *inputs;

    struct source *monitor_source;

    uint8_t volume;

    void (*notify)(struct sink*sink);
    uint32_t (*get_latency)(struct sink *s);
    void *userdata;
};

struct sink* sink_new(struct core *core, const char *name, const struct pa_sample_spec *spec);
void sink_free(struct sink* s);

int sink_render(struct sink*s, size_t length, struct memchunk *result);
int sink_render_into(struct sink*s, struct memchunk *target);
void sink_render_into_full(struct sink *s, struct memchunk *target);

uint32_t sink_get_latency(struct sink *s);

void sink_notify(struct sink*s);

char *sink_list_to_string(struct core *core);

struct sink* sink_get_default(struct core *c);



#endif
