#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "outputstream.h"

struct output_stream* output_stream_new(struct source *s, struct sample_spec *spec, const char *name) {
    struct output_stream *o;
    int r;
    assert(s && spec);

    o = malloc(sizeof(struct output_stream));
    assert(o);
    o->name = name ? strdup(name) : NULL;
    o->source = s;
    o->spec = *spec;
    o->kill = NULL;
    o->kill_userdata = NULL;

    o->memblockq = memblockq_new(bytes_per_second(spec)*5, sample_size(spec), (size_t) -1);
    assert(o->memblockq);
    
    assert(s->core);
    r = idxset_put(s->core->output_streams, o, &o->index);
    assert(r == 0 && o->index != IDXSET_INVALID);
    r = idxset_put(s->output_streams, o, NULL);
    assert(r == 0);
    
    return o;    
}

void output_stream_free(struct output_stream* o) {
    assert(o);

    memblockq_free(o->memblockq);

    assert(o->source && o->source->core);
    idxset_remove_by_data(o->source->core->output_streams, o, NULL);
    idxset_remove_by_data(o->source->output_streams, o, NULL);
    
    free(o->name);
    free(o);
}

void output_stream_set_kill_callback(struct output_stream *i, void (*kill)(struct output_stream*i, void *userdata), void *userdata) {
    assert(i && kill);
    i->kill = kill;
    i->kill_userdata = userdata;
}


void output_stream_kill(struct output_stream*i) {
    assert(i);

    if (i->kill)
        i->kill(i, i->kill_userdata);
}
