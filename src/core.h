#ifndef foocorehfoo
#define foocorehfoo

#include "idxset.h"
#include "mainloop-api.h"

struct core {
    struct pa_mainloop_api *mainloop;

    struct idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules;

    uint32_t default_source_index, default_sink_index;
};

struct core* core_new(struct pa_mainloop_api *m);
void core_free(struct core*c);

#endif
