#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "sourceoutput.h"

struct source* source_new(struct core *core, const char *name, const struct sample_spec *spec) {
    struct source *s;
    int r;
    assert(core && spec);

    s = malloc(sizeof(struct source));
    assert(s);

    s->name = name ? strdup(name) : NULL;
    s->core = core;
    s->sample_spec = *spec;
    s->outputs = idxset_new(NULL, NULL);

    s->notify = NULL;
    s->userdata = NULL;

    r = idxset_put(core->sources, s, &s->index);
    assert(s->index != IDXSET_INVALID && r >= 0);

    fprintf(stderr, "source: created %u \"%s\"\n", s->index, s->name);
    
    return s;
}

void source_free(struct source *s) {
    struct source_output *o, *j = NULL;
    assert(s);

    while ((o = idxset_first(s->outputs, NULL))) {
        assert(o != j);
        source_output_kill(o);
        j = o;
    }
    idxset_free(s->outputs, NULL, NULL);
    
    idxset_remove_by_data(s->core->sources, s, NULL);

    fprintf(stderr, "source: freed %u \"%s\"\n", s->index, s->name);

    free(s->name);
    free(s);
}

void source_notify(struct source*s) {
    assert(s);

    if (s->notify)
        s->notify(s);
}

static int do_post(void *p, uint32_t index, int *del, void*userdata) {
    struct memchunk *chunk = userdata;
    struct source_output *o = p;
    assert(o && o->push && index && del && chunk);

    o->push(o, chunk);
    return 0;
}

void source_post(struct source*s, struct memchunk *chunk) {
    assert(s && chunk);

    idxset_foreach(s->outputs, do_post, chunk);
}
