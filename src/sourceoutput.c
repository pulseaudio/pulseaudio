#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sourceoutput.h"

struct source_output* source_output_new(struct source *s, struct sample_spec *spec, const char *name) {
    struct source_output *o;
    int r;
    assert(s && spec);

    o = malloc(sizeof(struct source_output));
    assert(o);
    o->name = name ? strdup(name) : NULL;
    o->source = s;
    o->spec = *spec;

    o->push = NULL;
    o->kill = NULL;
    o->userdata = NULL;
    
    assert(s->core);
    r = idxset_put(s->core->source_outputs, o, &o->index);
    assert(r == 0 && o->index != IDXSET_INVALID);
    r = idxset_put(s->outputs, o, NULL);
    assert(r == 0);
    
    return o;    
}

void source_output_free(struct source_output* o) {
    assert(o);

    assert(o->source && o->source->core);
    idxset_remove_by_data(o->source->core->source_outputs, o, NULL);
    idxset_remove_by_data(o->source->outputs, o, NULL);
    
    free(o->name);
    free(o);
}

void source_output_kill(struct source_output*i) {
    assert(i);

    if (i->kill)
        i->kill(i);
}
