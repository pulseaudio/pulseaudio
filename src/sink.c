#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "sink.h"
#include "sinkinput.h"
#include "strbuf.h"

#define MAX_MIX_CHANNELS 32

struct sink* sink_new(struct core *core, const char *name, const struct sample_spec *spec) {
    struct sink *s;
    char *n = NULL;
    int r;
    assert(core && spec);

    s = malloc(sizeof(struct sink));
    assert(s);
    
    s->name = name ? strdup(name) : NULL;
    s->core = core;
    s->sample_spec = *spec;
    s->inputs = idxset_new(NULL, NULL);

    if (name) {
        n = malloc(strlen(name)+9);
        sprintf(n, "%s_monitor", name);
    }
    
    s->monitor_source = source_new(core, n, spec);
    free(n);
    
    s->volume = 0xFF;

    s->notify = NULL;
    s->get_latency = NULL;
    s->userdata = NULL;

    r = idxset_put(core->sinks, s, &s->index);
    assert(s->index != IDXSET_INVALID && r >= 0);
    
    fprintf(stderr, "sink: created %u \"%s\".\n", s->index, s->name);
    
    return s;
}

void sink_free(struct sink *s) {
    struct sink_input *i, *j = NULL;
    assert(s);

    while ((i = idxset_first(s->inputs, NULL))) {
        assert(i != j);
        sink_input_kill(i);
        j = i;
    }
    idxset_free(s->inputs, NULL, NULL);

    source_free(s->monitor_source);
    idxset_remove_by_data(s->core->sinks, s, NULL);

    fprintf(stderr, "sink: freed %u \"%s\"\n", s->index, s->name);
    
    free(s->name);
    free(s);
}

void sink_notify(struct sink*s) {
    assert(s);

    if (s->notify)
        s->notify(s);
}

static unsigned fill_mix_info(struct sink *s, struct mix_info *info, unsigned maxinfo) {
    uint32_t index = IDXSET_INVALID;
    struct sink_input *i;
    unsigned n = 0;
    
    assert(s && info);

    for (i = idxset_first(s->inputs, &index); maxinfo > 0 && i; i = idxset_next(s->inputs, &index)) {
        assert(i->peek);
        if (i->peek(i, &info->chunk) < 0)
            continue;

        info->volume = i->volume;
        
        assert(info->chunk.memblock && info->chunk.memblock->data && info->chunk.length);
        info->userdata = i;
        
        info++;
        maxinfo--;
        n++;
    }

    return n;
}

static void inputs_drop(struct sink *s, struct mix_info *info, unsigned maxinfo, size_t length) {
    assert(s && info);
    
    for (; maxinfo > 0; maxinfo--, info++) {
        struct sink_input *i = info->userdata;
        assert(i && info->chunk.memblock);
        
        memblock_unref(info->chunk.memblock);
        assert(i->drop);
        i->drop(i, length);
    }
}

int sink_render(struct sink*s, size_t length, struct memchunk *result) {
    struct mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    assert(s && length && result);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        return -1;

    if (n == 1) {
        struct sink_info *i = info[0].userdata;
        assert(i);
        *result = info[0].chunk;
        memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        l = result->length;
    } else {
        result->memblock = memblock_new(length);
        assert(result->memblock);

        result->length = l = mix_chunks(info, n, result->memblock->data, length, &s->sample_spec, s->volume);
        result->index = 0;
        
        assert(l);
    }

    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    source_post(s->monitor_source, result);

    return 0;
}

int sink_render_into(struct sink*s, struct memchunk *target) {
    struct mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    assert(s && target && target->length && target->memblock && target->memblock->data);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        return -1;

    if (n == 1) {
        struct sink_info *i = info[0].userdata;
        assert(i);

        l = target->length;
        if (l > info[0].chunk.length)
            l = info[0].chunk.length;
        
        memcpy(target->memblock->data+target->index, info[0].chunk.memblock->data + info[0].chunk.index, l);
        target->length = l;
    } else
        target->length = l = mix_chunks(info, n, target->memblock->data+target->index, target->length, &s->sample_spec, s->volume);
    
    assert(l);
    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    source_post(s->monitor_source, target);

    return 0;
}

void sink_render_into_full(struct sink *s, struct memchunk *target) {
    struct memchunk chunk;
    size_t l, d;
    assert(s && target && target->memblock && target->length && target->memblock->data);

    l = target->length;
    d = 0;
    while (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        
        if (sink_render_into(s, &chunk) < 0)
            break;

        d += chunk.length;
        l -= chunk.length;
    }

    if (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        silence_memchunk(&chunk, &s->sample_spec);
    }
}

uint32_t sink_get_latency(struct sink *s) {
    assert(s);

    if (!s->get_latency)
        return 0;

    return s->get_latency(s);
}

struct sink* sink_get_default(struct core *c) {
    struct sink *sink;
    assert(c);

    if ((sink = idxset_get_by_index(c->sinks, c->default_sink_index)))
        return sink;

    if (!(sink = idxset_first(c->sinks, &c->default_sink_index)))
        return NULL;

    fprintf(stderr, "core: default sink vanished, setting to %u.\n", sink->index);
    return sink;
}

char *sink_list_to_string(struct core *c) {
    struct strbuf *s;
    struct sink *sink, *default_sink;
    uint32_t index = IDXSET_INVALID;
    assert(c);

    s = strbuf_new();
    assert(s);

    strbuf_printf(s, "%u sink(s) available.\n", idxset_ncontents(c->sinks));

    default_sink = sink_get_default(c);
    
    for (sink = idxset_first(c->sinks, &index); sink; sink = idxset_next(c->sinks, &index)) {
        assert(sink->monitor_source);
        strbuf_printf(s, "  %c index: %u, name: <%s>, volume: <0x%02x>, latency: <%u usec>, monitor_source: <%u>\n", sink == default_sink ? '*' : ' ', sink->index, sink->name, (unsigned) sink->volume, sink_get_latency(sink), sink->monitor_source->index);
    }
    
    return strbuf_tostring_free(s);
}
