#ifndef foocorehfoo
#define foocorehfoo

#include "idxset.h"
#include "hashset.h"
#include "mainloop-api.h"

struct pa_core {
    struct pa_mainloop_api *mainloop;

    struct pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules;

    struct pa_hashset *namereg;
    
    uint32_t default_source_index, default_sink_index;
};

struct pa_core* pa_core_new(struct pa_mainloop_api *m);
void pa_core_free(struct pa_core*c);

#endif
