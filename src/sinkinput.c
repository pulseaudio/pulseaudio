#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sinkinput.h"
#include "strbuf.h"
#include "sample-util.h"

#define CONVERT_BUFFER_LENGTH 4096

struct sink_input* sink_input_new(struct sink *s, const char *name, const struct pa_sample_spec *spec) {
    struct sink_input *i;
    struct resampler *resampler = NULL;
    int r;
    assert(s && spec);

    if (!pa_sample_spec_equal(spec, &s->sample_spec))
        if (!(resampler = resampler_new(spec, &s->sample_spec)))
            return NULL;
    
    i = malloc(sizeof(struct sink_input));
    assert(i);
    i->name = name ? strdup(name) : NULL;
    i->sink = s;
    i->sample_spec = *spec;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->userdata = NULL;

    i->volume = VOLUME_NORM;

    i->resampled_chunk.memblock = NULL;
    i->resampled_chunk.index = i->resampled_chunk.length = 0;
    i->resampler = resampler;
    
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

    if (i->resampled_chunk.memblock)
        memblock_unref(i->resampled_chunk.memblock);
    if (i->resampler)
        resampler_free(i->resampler);
    
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
        strbuf_printf(s, "    index: %u, name: <%s>, sink: <%u>; volume: <0x%04x>, latency: <%u usec>\n",
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


int sink_input_peek(struct sink_input *i, struct memchunk *chunk) {
    assert(i && chunk && i->peek && i->drop);

    if (!i->resampler)
        return i->peek(i, chunk);

    if (!i->resampled_chunk.memblock) {
        struct memchunk tchunk;
        size_t l;
        int ret;
        
        if ((ret = i->peek(i, &tchunk)) < 0)
            return ret;

        l = resampler_request(i->resampler, CONVERT_BUFFER_LENGTH);
        if (tchunk.length > l)
            tchunk.length = l;

        i->drop(i, tchunk.length);
        
        resampler_run(i->resampler, &tchunk, &i->resampled_chunk);
        memblock_unref(tchunk.memblock);
    }

    assert(i->resampled_chunk.memblock && i->resampled_chunk.length);
    *chunk = i->resampled_chunk;
    memblock_ref(i->resampled_chunk.memblock);
    return 0;
}

void sink_input_drop(struct sink_input *i, size_t length) {
    assert(i && length);

    if (!i->resampler) {
        i->drop(i, length);
        return;
    }
    
    assert(i->resampled_chunk.memblock && i->resampled_chunk.length >= length);

    i->resampled_chunk.index += length;
    i->resampled_chunk.length -= length;

    if (!i->resampled_chunk.length) {
        memblock_unref(i->resampled_chunk.memblock);
        i->resampled_chunk.memblock = NULL;
        i->resampled_chunk.index = i->resampled_chunk.length = 0;
    }
}
