#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "core.h"
#include "module.h"
#include "sink.h"
#include "source.h"
#include "namereg.h"

struct core* core_new(struct pa_mainloop_api *m) {
    struct core* c;
    c = malloc(sizeof(struct core));
    assert(c);

    c->mainloop = m;
    c->clients = idxset_new(NULL, NULL);
    c->sinks = idxset_new(NULL, NULL);
    c->sources = idxset_new(NULL, NULL);
    c->source_outputs = idxset_new(NULL, NULL);
    c->sink_inputs = idxset_new(NULL, NULL);

    c->default_source_index = c->default_sink_index = IDXSET_INVALID;

    c->modules = NULL;
    c->namereg = NULL;
    
    return c;
};

void core_free(struct core *c) {
    assert(c);

    module_unload_all(c);
    assert(!c->modules);
    
    assert(idxset_isempty(c->clients));
    idxset_free(c->clients, NULL, NULL);
    
    assert(idxset_isempty(c->sinks));
    idxset_free(c->sinks, NULL, NULL);

    assert(idxset_isempty(c->sources));
    idxset_free(c->sources, NULL, NULL);
    
    assert(idxset_isempty(c->source_outputs));
    idxset_free(c->source_outputs, NULL, NULL);
    
    assert(idxset_isempty(c->sink_inputs));
    idxset_free(c->sink_inputs, NULL, NULL);

    namereg_free(c);
    
    free(c);    
};

