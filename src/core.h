#ifndef foocorehfoo
#define foocorehfoo

#include "idxset.h"
#include "mainloop.h"

struct core {
    struct mainloop *mainloop;

    struct idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules;

    uint32_t default_source_index, default_sink_index;
};

struct core* core_new(struct mainloop *m);
void core_free(struct core*c);

struct sink* core_get_default_sink(struct core *c);
struct source* core_get_default_source(struct core *c);

#endif
