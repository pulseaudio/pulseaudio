#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sourceoutput.h"
#include "strbuf.h"

struct pa_source_output* pa_source_output_new(struct pa_source *s, const char *name, const struct pa_sample_spec *spec) {
    struct pa_source_output *o;
    struct pa_resampler *resampler = NULL;
    int r;
    assert(s && spec);

    if (!pa_sample_spec_equal(&s->sample_spec, spec))
        if (!(resampler = pa_resampler_new(&s->sample_spec, spec)))
            return NULL;
    
    o = malloc(sizeof(struct pa_source_output));
    assert(o);
    o->name = name ? strdup(name) : NULL;
    o->source = s;
    o->sample_spec = *spec;

    o->push = NULL;
    o->kill = NULL;
    o->userdata = NULL;
    o->resampler = resampler;
    
    assert(s->core);
    r = pa_idxset_put(s->core->source_outputs, o, &o->index);
    assert(r == 0 && o->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->outputs, o, NULL);
    assert(r == 0);
    
    return o;    
}

void pa_source_output_free(struct pa_source_output* o) {
    assert(o);

    assert(o->source && o->source->core);
    pa_idxset_remove_by_data(o->source->core->source_outputs, o, NULL);
    pa_idxset_remove_by_data(o->source->outputs, o, NULL);

    if (o->resampler)
        pa_resampler_free(o->resampler);
    
    free(o->name);
    free(o);
}

void pa_source_output_kill(struct pa_source_output*i) {
    assert(i);

    if (i->kill)
        i->kill(i);
}

char *pa_source_output_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_source_output *o;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u source outputs(s) available.\n", pa_idxset_ncontents(c->source_outputs));

    for (o = pa_idxset_first(c->source_outputs, &index); o; o = pa_idxset_next(c->source_outputs, &index)) {
        assert(o->source);
        pa_strbuf_printf(s, "  %c index: %u, name: <%s>, source: <%u>\n",
                         o->index,
                         o->name,
                         o->source->index);
    }
    
    return pa_strbuf_tostring_free(s);
}

void pa_source_output_push(struct pa_source_output *o, const struct pa_memchunk *chunk) {
    struct pa_memchunk rchunk;
    assert(o && chunk && chunk->length && o->push);

    if (!o->resampler) {
        o->push(o, chunk);
        return;
    }

    pa_resampler_run(o->resampler, chunk, &rchunk);
    if (!rchunk.length)
        return;
    
    assert(rchunk.memblock);
    o->push(o, &rchunk);
    pa_memblock_unref(rchunk.memblock);
}
