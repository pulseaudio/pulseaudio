#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "sourceoutput.h"
#include "strbuf.h"
#include "namereg.h"

struct pa_source* pa_source_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec) {
    struct pa_source *s;
    char st[256];
    int r;
    assert(core && spec);

    s = malloc(sizeof(struct pa_source));
    assert(s);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SOURCE, s, fail))) {
        free(s);
        return NULL;
    }

    s->name = strdup(name);
    s->core = core;
    s->sample_spec = *spec;
    s->outputs = pa_idxset_new(NULL, NULL);
    s->monitor_of = NULL;

    s->notify = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sources, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_snprint(st, sizeof(st), spec);
    fprintf(stderr, "source: created %u \"%s\" with sample spec \"%s\"\n", s->index, s->name, st);
    
    return s;
}

void pa_source_free(struct pa_source *s) {
    struct pa_source_output *o, *j = NULL;
    assert(s);

    pa_namereg_unregister(s->core, s->name);
    
    while ((o = pa_idxset_first(s->outputs, NULL))) {
        assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }
    pa_idxset_free(s->outputs, NULL, NULL);
    
    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    fprintf(stderr, "source: freed %u \"%s\"\n", s->index, s->name);

    free(s->name);
    free(s);
}

void pa_source_notify(struct pa_source*s) {
    assert(s);

    if (s->notify)
        s->notify(s);
}

static int do_post(void *p, uint32_t index, int *del, void*userdata) {
    struct pa_memchunk *chunk = userdata;
    struct pa_source_output *o = p;
    assert(o && o->push && del && chunk);

    pa_source_output_push(o, chunk);
    return 0;
}

void pa_source_post(struct pa_source*s, struct pa_memchunk *chunk) {
    assert(s && chunk);

    pa_idxset_foreach(s->outputs, do_post, chunk);
}

struct pa_source* pa_source_get_default(struct pa_core *c) {
    struct pa_source *source;
    assert(c);

    if ((source = pa_idxset_get_by_index(c->sources, c->default_source_index)))
        return source;

    if (!(source = pa_idxset_first(c->sources, &c->default_source_index)))
        return NULL;

    fprintf(stderr, "core: default source vanished, setting to %u.\n", source->index);
    return source;
}

char *pa_source_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_source *source, *default_source;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u source(s) available.\n", pa_idxset_ncontents(c->sources));

    default_source = pa_source_get_default(c);
    
    for (source = pa_idxset_first(c->sources, &index); source; source = pa_idxset_next(c->sources, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        char mo[256] = "";
        if (source->monitor_of) 
            snprintf(mo, sizeof(mo), "\n\tmonitor_of: <%u>", source->monitor_of->index);
        pa_sample_snprint(ss, sizeof(ss), &source->sample_spec);
        pa_strbuf_printf(s, "  %c index: %u\n\tname: <%s>\n\tsample_spec: <%s>%s\n", source == default_source ? '*' : ' ', source->index, source->name, ss, mo);
    }
    
    return pa_strbuf_tostring_free(s);
}

