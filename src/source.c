#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "outputstream.h"

struct source* source_new(struct core *core, const char *name, const struct sample_spec *spec) {
    struct source *s;
    int r;
    assert(core && spec);

    s = malloc(sizeof(struct source));
    assert(s);

    s->name = name ? strdup(name) : NULL;
    r = idxset_put(core->sources, s, &s->index);
    assert(s->index != IDXSET_INVALID && r >= 0);

    s->core = core;
    s->sample_spec = *spec;
    s->output_streams = idxset_new(NULL, NULL);

    s->link_change_callback = NULL;
    s->userdata = NULL;

    return s;
}

static void do_free(void *p, void *userdata) {
    struct output_stream *o = p;
    assert(o);
    output_stream_free(o);
};

void source_free(struct source *s) {
    assert(s);

    idxset_remove_by_data(s->core->sources, s, NULL);
    idxset_free(s->output_streams, do_free, NULL);
    free(s->name);
    free(s);
}

static int do_post(void *p, uint32_t index, int *del, void*userdata) {
    struct memchunk *chunk = userdata;
    struct output_stream *o = p;
    assert(o && o->memblockq && index && del && chunk);

    memblockq_push(o->memblockq, chunk, 0);
    return 0;
}

void source_post(struct source*s, struct memchunk *chunk) {
    assert(s && chunk);

    idxset_foreach(s->output_streams, do_post, chunk);
}
