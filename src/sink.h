#ifndef foosinkhfoo
#define foosinkhfoo

struct sink;

#include <inttypes.h>

#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "source.h"

struct sink {
    char *name;
    uint32_t index;
    
    struct core *core;
    struct sample_spec sample_spec;
    struct idxset *input_streams;

    struct source *monitor_source;

    uint8_t volume;

    void (*notify_callback)(struct sink*sink, void *userdata);
    void *userdata;
};

struct sink* sink_new(struct core *core, const char *name, const struct sample_spec *spec);
void sink_free(struct sink* s);

int sink_render(struct sink*s, size_t length, struct memchunk *result);
int sink_render_into(struct sink*s, struct memblock *target, struct memchunk *result);

void sink_notify(struct sink*s);
void sink_set_notify_callback(struct sink *s, void (*notify_callback)(struct sink*sink, void *userdata), void *userdata);

#endif
