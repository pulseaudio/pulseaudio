#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "core.h"
#include "module.h"
#include "sink.h"
#include "source.h"
#include "namereg.h"
#include "util.h"

struct pa_core* pa_core_new(struct pa_mainloop_api *m) {
    struct pa_core* c;
    c = malloc(sizeof(struct pa_core));
    assert(c);

    c->mainloop = m;
    c->clients = pa_idxset_new(NULL, NULL);
    c->sinks = pa_idxset_new(NULL, NULL);
    c->sources = pa_idxset_new(NULL, NULL);
    c->source_outputs = pa_idxset_new(NULL, NULL);
    c->sink_inputs = pa_idxset_new(NULL, NULL);

    c->default_source_index = c->default_sink_index = PA_IDXSET_INVALID;

    c->modules = NULL;
    c->namereg = NULL;

    c->default_sample_spec.format = PA_SAMPLE_S16NE;
    c->default_sample_spec.rate = 44100;
    c->default_sample_spec.channels = 2;
    
    pa_check_for_sigpipe();
    
    return c;
};

void pa_core_free(struct pa_core *c) {
    assert(c);

    pa_module_unload_all(c);
    assert(!c->modules);
    
    assert(pa_idxset_isempty(c->clients));
    pa_idxset_free(c->clients, NULL, NULL);
    
    assert(pa_idxset_isempty(c->sinks));
    pa_idxset_free(c->sinks, NULL, NULL);

    assert(pa_idxset_isempty(c->sources));
    pa_idxset_free(c->sources, NULL, NULL);
    
    assert(pa_idxset_isempty(c->source_outputs));
    pa_idxset_free(c->source_outputs, NULL, NULL);
    
    assert(pa_idxset_isempty(c->sink_inputs));
    pa_idxset_free(c->sink_inputs, NULL, NULL);

    pa_namereg_free(c);
    
    free(c);    
};

