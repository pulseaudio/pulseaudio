#ifndef foocorehfoo
#define foocorehfoo

#include "idxset.h"
#include "hashmap.h"
#include "mainloop-api.h"
#include "sample.h"

struct pa_core {
    struct pa_mainloop_api *mainloop;

    struct pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules;

    struct pa_hashmap *namereg;
    
    uint32_t default_source_index, default_sink_index;

    struct pa_sample_spec default_sample_spec;
};

struct pa_core* pa_core_new(struct pa_mainloop_api *m);
void pa_core_free(struct pa_core*c);

#endif
