#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sinkinput.h"
#include "strbuf.h"

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
    i->get_latency = NULL;
    i->userdata = NULL;

    i->volume = 0xFF;

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

char *sink_input_list_to_string(struct core *c) {
    struct strbuf *s;
    struct sink_input *i;
    uint32_t index = IDXSET_INVALID;
    assert(c);

    s = strbuf_new();
    assert(s);

    strbuf_printf(s, "%u sink input(s) available.\n", idxset_ncontents(c->sink_inputs));

    for (i = idxset_first(c->sink_inputs, &index); i; i = idxset_next(c->sink_inputs, &index)) {
        assert(i->sink);
        strbuf_printf(s, "    index: %u, name: <%s>, sink: <%u>; volume: <0x%02x>, latency: <%u usec>\n",
                      i->index,
                      i->name,
                      i->sink->index,
                      (unsigned) i->volume,
                      sink_input_get_latency(i));
    }
    
    return strbuf_tostring_free(s);
}

uint32_t sink_input_get_latency(struct sink_input *i) {
    uint32_t l = 0;
    
    assert(i);
    if (i->get_latency)
        l += i->get_latency(i);

    assert(i->sink);
    l += sink_get_latency(i->sink);

    return l;
}
