#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sinkinput.h"

struct sink_input* sink_input_new(struct sink *s, struct sample_spec *spec, const char *name) {
    struct sink_input *i;
    int r;
    assert(s && spec);

    i = malloc(sizeof(struct sink_input));
    assert(i);
    i->name = name ? strdup(name) : NULL;
    i->sink = s;
    i->spec = *spec;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->userdata = NULL;

    assert(s->core);
    r = idxset_put(s->core->sink_inputs, i, &i->index);
    assert(r == 0 && i->index != IDXSET_INVALID);
    r = idxset_put(s->inputs, i, NULL);
    assert(r == 0);
    
    return i;    
}

void sink_input_free(struct sink_input* i) {
    assert(i);

    assert(i->sink && i->sink->core);
    idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    idxset_remove_by_data(i->sink->inputs, i, NULL);
    
    free(i->name);
    free(i);
}

void sink_input_kill(struct sink_input*i) {
    assert(i);

    if (i->kill)
        i->kill(i);
}
